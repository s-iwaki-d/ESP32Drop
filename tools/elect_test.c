/* Host tests for awdl_elect.h -- the SAME choice code the firmware runs.
 *
 * This is the file that was missing when the two worst failures in this project
 * happened. Both were election bugs, both reached the device, and both were caught
 * by a human noticing the badge had disappeared from AirDrop. The rules encoded here
 * are the ones that cost something to learn; each test names what it is protecting.
 *
 *   cc -O2 -o /tmp/elect_test tools/elect_test.c && /tmp/elect_test
 */
#include <stdio.h>
#include <string.h>
#include "../ESP32Drop/src/awdl/core/awdl_elect.h"

static int fails = 0, ran = 0;
static void check(const char *name, int ok, const char *detail) {
  ran++;
  printf("  [%s] %-58s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}

static const uint8_t OWN[6]  = {0x2a,0x84,0x85,0x43,0xb3,0x0c};   /* the badge */
static const uint8_t MAC_A[6] = {0xe2,0xa6,0xfb,0xcd,0xa8,0xa4};  /* the Mac */
static const uint8_t MAC_B[6] = {0xce,0x57,0xf1,0xaf,0x9d,0x17};  /* iPhone Air */
static const uint8_t MAC_C[6] = {0xda,0x8f,0xd6,0x15,0x36,0xc2};  /* iPad */

/* A healthy, recently heard candidate. `candu` sets the re-baser ban; `cand` is the
   trusted shorthand -- C has no default arguments. */
static struct ElectCand candu(const uint8_t *mac, uint32_t gap, uint32_t metric,
                              uint32_t last, bool untrusted) {
  struct ElectCand c; memset(&c, 0, sizeof c);
  c.used = true; memcpy(c.addr, mac, 6);
  c.n = 16; c.gap_avg = gap; c.metric = metric; c.last_ms = last;
  c.stdev = 100.0f; c.untrusted = untrusted;
  return c;
}
static struct ElectCand cand(const uint8_t *mac, uint32_t gap, uint32_t metric,
                             uint32_t last) {
  return candu(mac, gap, metric, last, false);
}

int main(void) {
  printf("== awdl_elect.h host tests (master election) ==\n");
  const uint32_t NOW = 100000;

  /* 1. Consensus, not metric: the master announced most often wins even though a
     rival claims a bigger number. Metric-first flapped 842 times in four minutes
     and took the badge out of the Mac's peer table. */
  {
    struct ElectCand c[2] = { cand(MAC_A, 200, 510, NOW), cand(MAC_B, 90, 540, NOW) };
    struct ElectResult r = elect_choose(c, 2, NOW, OWN, 0, false);
    check("lowest announcement gap wins, not the biggest metric",
          r.chosen == 1 && r.count == 2, r.chosen == 1 ? "chose the gap=90 peer" : "wrong winner");
  }

  /* 2. Metric only breaks a tie. */
  {
    struct ElectCand c[2] = { cand(MAC_A, 200, 510, NOW), cand(MAC_B, 200, 540, NOW) };
    struct ElectResult r = elect_choose(c, 2, NOW, OWN, 0, false);
    check("equal gaps -> higher metric breaks the tie", r.chosen == 1, NULL);
  }

  /* 3. Hysteresis: a marginally better challenger must NOT move us, because every
     switch changes the master_addr we advertise and that is what the Mac judges. */
  {
    struct ElectCand c[2] = { cand(MAC_A, 200, 510, NOW), cand(MAC_B, 150, 510, NOW) };
    struct ElectResult r = elect_choose(c, 2, NOW, OWN, MAC_A, true);
    check("incumbent held against a merely-slightly-better challenger",
          r.chosen == 0, r.chosen == 0 ? "stayed" : "flapped");
  }

  /* 4. ...but a clearly better one does take over (1.5x). */
  {
    struct ElectCand c[2] = { cand(MAC_A, 300, 510, NOW), cand(MAC_B, 100, 510, NOW) };
    struct ElectResult r = elect_choose(c, 2, NOW, OWN, MAC_A, true);
    check("a clearly-better challenger (>1.5x) does win", r.chosen == 1, NULL);
  }

  /* 5. The incumbent survives a longer silence than a stranger (15s vs 5s). */
  {
    struct ElectCand c[2] = { cand(MAC_A, 200, 510, NOW - 9000),
                              cand(MAC_B, 100, 510, NOW - 9000) };
    struct ElectResult r = elect_choose(c, 2, NOW, OWN, MAC_A, true);
    check("at 9s silence only the incumbent is still eligible",
          r.chosen == 0 && r.count == 1, r.count == 1 ? "1 eligible" : "wrong count");
  }

  /* 6. Too few samples is not evidence. */
  {
    struct ElectCand c[1] = { cand(MAC_A, 100, 510, NOW) };
    c[0].n = 3;
    struct ElectResult r = elect_choose(c, 1, NOW, OWN, 0, false);
    check("a row with <4 samples is not eligible", r.chosen == -1 && r.count == 0, NULL);
  }

  /* 7. THE SELF-ELECTION GUARD. A foreign peer naming us in its sync-params creates
     a row for our own MAC; electing it makes the gauge measure our clock against
     itself -- measured collapsing gauge health 30 -> 0 for 24 seconds. */
  {
    struct ElectCand c[2] = { cand(OWN, 50, 600, NOW),      /* best on every metric */
                              cand(MAC_A, 200, 510, NOW) };
    struct ElectResult r = elect_choose(c, 2, NOW, OWN, 0, false);
    check("our own address is refused even when it looks best",
          r.chosen == 1 && r.self_seen && r.count == 1,
          r.self_seen ? "refused and reported" : "NOT refused");
  }

  /* 8. Trusted candidates keep re-basers out of the running entirely. */
  {
    struct ElectCand c[2] = { candu(MAC_B, 50, 540, NOW, true),   /* re-baser, best gap */
                              candu(MAC_A, 200, 510, NOW, false) };
    struct ElectResult r = elect_choose(c, 2, NOW, OWN, 0, false);
    check("a re-baser loses to a trusted peer and does not trigger the fallback",
          r.chosen == 1 && !r.fallback, r.fallback ? "fallback fired wrongly" : "clean");
  }

  /* 9. THE FALLBACK. Every candidate banned -> elect one anyway rather than go
     silent. This is the 74s outage: no master means send_mif is never called, and
     macOS ages us out of its neighbour table after ~6-8s of silence. */
  {
    struct ElectCand c[2] = { candu(MAC_B, 200, 540, NOW, true),
                              candu(MAC_C, 100, 510, NOW, true) };
    struct ElectResult r = elect_choose(c, 2, NOW, OWN, 0, false);
    check("all candidates banned -> elect the best of them, flagged as fallback",
          r.chosen == 1 && r.fallback && r.count == 2,
          r.chosen >= 0 ? "elected instead of going silent" : "went silent");
  }

  /* 10. The fallback must not resurrect the ineligible: stale and self stay out
     even on the second pass, or "last resort" would become "no rules at all". */
  {
    struct ElectCand c[3] = { candu(MAC_B, 100, 540, NOW - 20000, true),  /* stale */
                              candu(OWN,   50, 600, NOW,         true),  /* us */
                              candu(MAC_C, 300, 510, NOW,        true) };
    struct ElectResult r = elect_choose(c, 3, NOW, OWN, 0, false);
    check("fallback still excludes stale rows and our own address",
          r.chosen == 2 && r.self_seen && r.count == 1, NULL);
  }

  /* 11. Nobody at all: report it rather than inventing a winner. */
  {
    struct ElectCand c[1] = { cand(MAC_A, 100, 510, NOW - 30000) };
    struct ElectResult r = elect_choose(c, 1, NOW, OWN, 0, false);
    check("no eligible candidate -> chosen=-1 (a real gap, honestly reported)",
          r.chosen == -1 && r.count == 0 && !r.self_seen, NULL);
  }

  /* 12. An unused slot is not a candidate. */
  {
    struct ElectCand c[2]; memset(c, 0, sizeof c);
    c[1] = cand(MAC_A, 100, 510, NOW);
    struct ElectResult r = elect_choose(c, 2, NOW, OWN, 0, false);
    check("empty table slots are skipped", r.chosen == 1 && r.count == 1, NULL);
  }

  /* 13. The incumbent being untrusted must not deadlock the election: somebody
     trusted still wins pass 0. */
  {
    struct ElectCand c[2] = { candu(MAC_B, 100, 540, NOW, true),
                              candu(MAC_A, 400, 510, NOW, false) };
    struct ElectResult r = elect_choose(c, 2, NOW, OWN, MAC_B, true);
    check("an untrusted incumbent does not block a trusted challenger",
          r.chosen == 1 && !r.fallback, NULL);
  }

  printf("\n%d checks, %d failed\n", ran, fails);
  return fails ? 1 : 0;
}
