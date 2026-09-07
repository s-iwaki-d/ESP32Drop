/* Host tests for awdl_ring.h -- the SPSC cursor both rings share.
 *
 * What is actually at stake here is not "does a ring buffer work". It is that the frame
 * ring's producer runs in the WiFi callback on core 0 and must never block, and the dump
 * ring's consumer holds a UART measured stalling for 250 ms. If full is ever mistaken for
 * empty the consumer reads a slot the producer is still filling; if empty is ever mistaken
 * for full the producer drops frames that would have fit. Both failures are silent.
 *
 *   cc -O2 -o /tmp/ring_test tools/ring_test.c && /tmp/ring_test
 */
#include <stdio.h>
#include <string.h>
#include "../ESP32Drop/src/awdl/core/awdl_ring.h"

static int fails = 0, ran = 0;
static void check(const char *name, int ok, const char *detail) {
  ran++;
  printf("  [%s] %-58s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}

int main(void) {
  printf("== awdl_ring.h host tests ==\n");
  struct AwdlRing r;

  /* 1. Empty and full, and the one slot spent to tell them apart. */
  {
    awdl_ring_init(&r, 4);
    check("a fresh ring is empty", awdl_ring_peek(&r) == -1, NULL);
    check("...and reports nothing used", awdl_ring_used(&r) == 0, NULL);
    int got = 0;
    for (int i = 0; i < 3; i++) { if (awdl_ring_reserve(&r) >= 0) { awdl_ring_commit(&r); got++; } }
    check("4 slots hold 3 entries -- one is spent distinguishing full from empty",
          got == 3, NULL);
    check("...and the ring says so", awdl_ring_used(&r) == 3, NULL);
    check("the 4th reserve fails rather than overwriting", awdl_ring_reserve(&r) == -1, NULL);
    check("...and is counted, because a silent drop is the failure mode that hides",
          r.drops == 1, NULL);
  }

  /* 2. Reserve hands out the slot the producer should fill, and commit is what makes it
     visible -- not reserve. A consumer that could see a reserved-but-uncommitted slot
     would be reading a half-written frame. */
  {
    awdl_ring_init(&r, 8);
    int i = awdl_ring_reserve(&r);
    check("reserve returns slot 0 on an empty ring", i == 0, NULL);
    check("the ring is still EMPTY after reserve alone -- commit publishes, not reserve",
          awdl_ring_peek(&r) == -1, NULL);
    awdl_ring_commit(&r);
    check("after commit the consumer sees exactly that slot", awdl_ring_peek(&r) == 0, NULL);
    check("peek does not consume: asking twice gives the same slot",
          awdl_ring_peek(&r) == 0, NULL);
    awdl_ring_release(&r);
    check("release frees it and the ring is empty again", awdl_ring_peek(&r) == -1, NULL);
  }

  /* 3. FIFO, across the wrap. The frame ring's whole purpose is to preserve arrival order
     for the phase estimator; a reordered ring would feed the PLL samples out of time. */
  {
    awdl_ring_init(&r, 8);
    int payload[8], out[4096], n_out = 0, seq = 0, bad_slot = 0;
    memset(payload, 0, sizeof payload);
    for (int round = 0; round < 1000; round++) {
      for (int k = 0; k < 3; k++) {
        int i = awdl_ring_reserve(&r);
        if (i < 0) continue;
        if (i < 0 || i > 7) bad_slot = 1;
        payload[i] = seq++;
        awdl_ring_commit(&r);
      }
      for (int k = 0; k < 2; k++) {
        int i = awdl_ring_peek(&r);
        if (i < 0) break;
        if (i < 0 || i > 7) bad_slot = 1;
        if (n_out < 4096) out[n_out++] = payload[i];
        awdl_ring_release(&r);
      }
    }
    check("every index handed out is inside the array", !bad_slot, NULL);
    int ordered = 1;
    for (int i = 1; i < n_out; i++) if (out[i] != out[i-1] + 1) ordered = 0;
    char d[64]; snprintf(d, sizeof d, "%d entries through an 8-slot ring", n_out);
    check("entries come out in the order they went in, across many wraps", ordered && n_out > 1000, d);
  }

  /* 4. The cursors are free-running counters masked at use, so nothing special happens
     when head or tail passes the top of the array -- but that is worth demonstrating
     rather than trusting, since it is the case a hand-written ring gets wrong. */
  {
    awdl_ring_init(&r, 2);           /* capacity 1: maximum wrap pressure */
    int ok = 1;
    for (int i = 0; i < 10000; i++) {
      if (awdl_ring_reserve(&r) < 0) { ok = 0; break; }
      awdl_ring_commit(&r);
      if (awdl_ring_peek(&r) < 0)    { ok = 0; break; }
      awdl_ring_release(&r);
    }
    check("a 2-slot ring cycles 10,000 times without wedging", ok && r.drops == 0, NULL);
  }

  /* 5. Drops accumulate and do not disturb the cursors. When the consumer is behind, the
     producer must keep dropping cleanly rather than corrupting what is already queued --
     this is the state the frame ring is in whenever proc_max spikes. */
  {
    awdl_ring_init(&r, 4);
    int payload[4];
    for (int i = 0; i < 3; i++) { int s = awdl_ring_reserve(&r); payload[s] = 100 + i; awdl_ring_commit(&r); }
    for (int i = 0; i < 50; i++) { if (awdl_ring_reserve(&r) != -1) { check("reserve should fail", 0, NULL); break; } }
    check("50 further reserves all fail and are all counted", r.drops == 50, NULL);
    int a = awdl_ring_peek(&r); int v0 = payload[a]; awdl_ring_release(&r);
    int b = awdl_ring_peek(&r); int v1 = payload[b]; awdl_ring_release(&r);
    check("the queued entries survived the drop storm intact", v0 == 100 && v1 == 101, NULL);
    check("...and a slot is available again once the consumer catches up",
          awdl_ring_reserve(&r) >= 0, NULL);
  }

  /* 6. used() is a diagnostic. On the live rings it reads two cursors the other core is
     moving, so it is a snapshot -- but it must at least be right when nobody is moving. */
  {
    awdl_ring_init(&r, 16);
    for (int i = 0; i < 10; i++) { awdl_ring_reserve(&r); awdl_ring_commit(&r); }
    check("used() counts committed entries", awdl_ring_used(&r) == 10, NULL);
    for (int i = 0; i < 4; i++) { awdl_ring_peek(&r); awdl_ring_release(&r); }
    check("...and drops back as they are consumed", awdl_ring_used(&r) == 6, NULL);
  }

  printf("\n%d checks, %d failed\n", ran, fails);
  return fails ? 1 : 0;
}
