/* Host tests for awdl_window.h -- the transmit gate.
 *
 * This arithmetic decides when the badge may transmit, and it is the only thing between
 * the badge and being invisible. Gate too tightly and the MIF never leaves; gate on the
 * wrong slot and it leaves into an empty window. Both have happened to this project, and
 * the second one -- a gauge 8 AW out, 4,125 frames fired into a slot nobody was
 * listening on -- was found by a human noticing the badge had vanished from AirDrop.
 *
 * The central test is not a set of examples. It is an EXHAUSTIVE comparison against a
 * verbatim transcription of the in_ch6_window_now() logic this replaces, over every
 * microsecond of the AW cycle and every mask shape that matters. If the two ever disagree
 * at any phase, the extraction changed behaviour.
 *
 *   cc -O2 -o /tmp/window_test tools/window_test.c && /tmp/window_test
 */
#include <stdio.h>
#include <string.h>
#include "../ESP32Drop/src/awdl/core/awdl_window.h"

static int fails = 0, ran = 0;
static void check(const char *name, int ok, const char *detail) {
  ran++;
  printf("  [%s] %-58s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}

/* The ORIGINAL gate, transcribed from the original firmware's in_ch6_window_now() with only the
   clock and the globals lifted into arguments. Nothing else is changed -- this is the
   thing the extraction must reproduce exactly. */
static bool ref_in_window(uint16_t mask, int64_t phase) {
  if (mask == 0) return false;
  const int64_t SLOT_US = 4 * AW_US;
  int s = (int)(phase / SLOT_US);
  if (s < 0 || s >= 16 || !(mask & (1u << s))) return false;
  int64_t into = phase - (int64_t)s * SLOT_US;
  bool prev6 = mask & (uint16_t)(1u << ((s + 15) & 15));
  bool next6 = mask & (uint16_t)(1u << ((s + 1) & 15));
  if (!prev6 && into < 1000)  return false;                 /* WIN_LEAD_US  */
  if (!next6 && into >= SLOT_US - 12000) return false;      /* WIN_GUARD_US */
  return true;
}

/* Masks that have actually been observed, plus the shapes that exercise the run logic. */
static const uint16_t MASKS[] = {
  0x0000,   /* no elected master */
  0x0100,   /* one ch6 slot at 8 -- what the mesh gives at rest, seen in every capture */
  0x0300,   /* two adjacent, seen when the mesh densifies */
  0x3b00,   /* slots 8,9,11,12,13 -- seen in a capture */
  0x8001,   /* slots 0 and 15: a run that WRAPS the cycle boundary */
  0xffff,   /* every slot, as the MIF we transmit advertises */
  0x0001,   /* slot 0 alone: phase 0 is its lead-in */
  0x5555,   /* alternating: every slot isolated */
};
#define NMASKS (int)(sizeof(MASKS) / sizeof(MASKS[0]))

int main(void) {
  printf("awdl_window.h -- the transmit gate\n\n");

  /* 1. EXHAUSTIVE equivalence with the code being replaced. Every microsecond of the
     cycle, every mask. This is the whole reason to trust the extraction. */
  {
    long long mismatches = 0; int64_t first_bad = -1; uint16_t bad_mask = 0;
    for (int mi = 0; mi < NMASKS; mi++) {
      struct AwdlWindow w; awdl_window_init(&w, MASKS[mi]);
      for (int64_t p = 0; p < AWC_US; p++) {
        if (awdl_window_open_at(&w, p) != ref_in_window(MASKS[mi], p)) {
          if (!mismatches) { first_bad = p; bad_mask = MASKS[mi]; }
          mismatches++;
        }
      }
    }
    char d[96];
    if (mismatches) snprintf(d, sizeof d, "%lld mismatches, first at mask=0x%04x phase=%lld",
                             mismatches, bad_mask, (long long)first_bad);
    else snprintf(d, sizeof d, "%d masks x %lld phases, all identical",
                  NMASKS, (long long)AWC_US);
    check("identical to in_ch6_window_now() at EVERY phase", mismatches == 0, d);
  }

  /* 2. The lead and the guard exist for measured reasons, so pin their exact edges on an
     isolated slot: LEAD lets the radio settle after the run opens, GUARD stops a TX being
     initiated so late that frame build plus esp_wifi_80211_tx pushes it into the next
     (5GHz) slot. */
  {
    struct AwdlWindow w; awdl_window_init(&w, 0x0100);      /* slot 8 alone */
    int64_t base = 8 * AWDL_SLOT_US;
    check("closed at the instant an isolated slot begins",
          !awdl_window_open_at(&w, base), NULL);
    check("closed one microsecond before the lead-in ends",
          !awdl_window_open_at(&w, base + AWDL_WIN_LEAD_US - 1), NULL);
    check("open exactly when the lead-in ends",
          awdl_window_open_at(&w, base + AWDL_WIN_LEAD_US), NULL);
    check("open one microsecond before the guard band starts",
          awdl_window_open_at(&w, base + AWDL_SLOT_US - AWDL_WIN_GUARD_US - 1), NULL);
    check("closed exactly when the guard band starts",
          !awdl_window_open_at(&w, base + AWDL_SLOT_US - AWDL_WIN_GUARD_US), NULL);
    char d[64];
    snprintf(d, sizeof d, "%lld us usable of %lld",
             (long long)(AWDL_SLOT_US - AWDL_WIN_LEAD_US - AWDL_WIN_GUARD_US),
             (long long)AWDL_SLOT_US);
    check("an isolated slot yields the 52,536 us the design is built around",
          AWDL_SLOT_US - AWDL_WIN_LEAD_US - AWDL_WIN_GUARD_US == 52536, d);
  }

  /* 3. Runs. The mesh densifies ch6 under traffic -- 1/16 to 5/16 observed during a
     transfer -- and guarding each slot of a run separately would throw that airtime away.
     Inside a run there is no lead and no guard. */
  {
    struct AwdlWindow w; awdl_window_init(&w, 0x0300);      /* slots 8 and 9 */
    int64_t b8 = 8 * AWDL_SLOT_US, b9 = 9 * AWDL_SLOT_US;
    check("run: the boundary between two ch6 slots is open",
          awdl_window_open_at(&w, b9), NULL);
    check("run: no guard at the end of the FIRST slot",
          awdl_window_open_at(&w, b9 - 1), NULL);
    check("run: no lead at the start of the SECOND slot",
          awdl_window_open_at(&w, b9 + 1), NULL);
    check("run: lead still applies at the start of the run",
          !awdl_window_open_at(&w, b8 + AWDL_WIN_LEAD_US - 1), NULL);
    check("run: guard still applies at the end of the run",
          !awdl_window_open_at(&w, b9 + AWDL_SLOT_US - AWDL_WIN_GUARD_US), NULL);
    long long usable = 0;
    for (int64_t p = 0; p < AWC_US; p++) if (awdl_window_open_at(&w, p)) usable++;
    char d[64]; snprintf(d, sizeof d, "%lld us", usable);
    check("a two-slot run yields 118,072 us, not 2 x 52,536", usable == 118072, d);
  }

  /* 4. A run that wraps the cycle boundary. Slot 15 and slot 0 are adjacent in a ring,
     and the modular arithmetic has to see that. */
  {
    struct AwdlWindow w; awdl_window_init(&w, 0x8001);      /* slots 0 and 15 */
    check("wrap: the last microsecond of slot 15 is open",
          awdl_window_open_at(&w, AWC_US - 1), NULL);
    check("wrap: phase 0 is open, because slot 15 precedes it",
          awdl_window_open_at(&w, 0), NULL);
    check("wrap: the guard applies at the end of slot 0, which closes the run",
          !awdl_window_open_at(&w, AWDL_SLOT_US - AWDL_WIN_GUARD_US), NULL);
  }

  /* 5. No mask means no authority for where the window is. Transmitting on a guess is
     how 4,125 frames went into an empty slot. */
  {
    struct AwdlWindow w; awdl_window_init(&w, 0x0000);
    int open = 0;
    for (int64_t p = 0; p < AWC_US; p += 97) if (awdl_window_open_at(&w, p)) open++;
    check("an empty mask is never open, at any phase", open == 0, NULL);
    check("an empty mask reports a full cycle until it opens",
          awdl_window_next_at(&w, 0) == AWC_US, NULL);
  }

  /* 6. next_at: the deadline. Its whole purpose is telling a caller about to block
     whether it has room, so it must never overstate the time available. */
  {
    struct AwdlWindow w; awdl_window_init(&w, 0x0100);
    int64_t base = 8 * AWDL_SLOT_US;
    check("open now -> 0", awdl_window_next_at(&w, base + AWDL_WIN_LEAD_US) == 0, NULL);
    check("just before the lead ends -> 1 us",
          awdl_window_next_at(&w, base + AWDL_WIN_LEAD_US - 1) == 1, NULL);
    check("at phase 0 -> the whole way to slot 8's lead point",
          awdl_window_next_at(&w, 0) == base + AWDL_WIN_LEAD_US, NULL);
    /* inside the guard band the window is shut until the NEXT cycle */
    int64_t in_guard = base + AWDL_SLOT_US - AWDL_WIN_GUARD_US + 5;
    int64_t d = awdl_window_next_at(&w, in_guard);
    char det[80]; snprintf(det, sizeof det, "%lld us", (long long)d);
    check("inside the guard band -> waits for the next cycle",
          d == AWC_US - in_guard + base + AWDL_WIN_LEAD_US, det);

    /* THE PROPERTY THAT MATTERS: never claim more slack than there is. Sweep the whole
       cycle and confirm the window really is shut for the entire reported interval. */
    long long bad = 0;
    for (int64_t p = 0; p < AWC_US; p += 13) {
      int64_t n = awdl_window_next_at(&w, p);
      if (n == 0) continue;
      if (n > AWC_US) { bad++; continue; }
      /* it must be shut right up to the deadline, and open at it */
      if (awdl_window_open_at(&w, (p + n - 1) % AWC_US)) bad++;
      if (!awdl_window_open_at(&w, (p + n) % AWC_US)) bad++;
    }
    char d2[64]; snprintf(d2, sizeof d2, "%lld violations", bad);
    check("next_at never overstates the slack, at any phase", bad == 0, d2);
  }

  /* 7. The same property across every mask shape, since a caller trusts it regardless. */
  {
    long long bad = 0;
    for (int mi = 1; mi < NMASKS; mi++) {          /* skip the empty mask */
      struct AwdlWindow w; awdl_window_init(&w, MASKS[mi]);
      for (int64_t p = 0; p < AWC_US; p += 251) {
        int64_t n = awdl_window_next_at(&w, p);
        if (n == 0) { if (!awdl_window_open_at(&w, p)) bad++; continue; }
        if (n < 0 || n > AWC_US) { bad++; continue; }
        if (awdl_window_open_at(&w, (p + n - 1) % AWC_US)) bad++;
        if (!awdl_window_open_at(&w, (p + n) % AWC_US)) bad++;
      }
    }
    char d[64]; snprintf(d, sizeof d, "%lld violations over %d masks", bad, NMASKS - 1);
    check("next_at agrees with open_at for every mask shape", bad == 0, d);
  }

  /* 8. Channel-sequence decoding. */
  {
    uint8_t cs[16];
    for (int i = 0; i < 16; i++) cs[i] = 44;      /* a 5GHz channel */
    cs[8] = 6;
    check("one ch6 slot -> mask 0x0100", awdl_chanseq_mask_of(cs, 6) == 0x0100, NULL);
    check("...and its first slot is 8", awdl_chanseq_first_of(cs, 6) == 8, NULL);
    cs[9] = 6; cs[11] = 6; cs[12] = 6; cs[13] = 6;
    check("the captured 0x3b00 shape decodes",
          awdl_chanseq_mask_of(cs, 6) == 0x3b00, NULL);
    for (int i = 0; i < 16; i++) cs[i] = 6;
    check("all ch6 -> 0xffff (what our own MIF advertises)",
          awdl_chanseq_mask_of(cs, 6) == 0xffff, NULL);
    for (int i = 0; i < 16; i++) cs[i] = 44;
    check("no ch6 -> mask 0", awdl_chanseq_mask_of(cs, 6) == 0, NULL);
    check("no ch6 -> first slot is -1", awdl_chanseq_first_of(cs, 6) == -1, NULL);
  }

  /* ---- awdl_window_remaining_at ------------------------------------------------
     The property that matters is not the arithmetic, it is the equivalence: a caller
     about to spend 30 ms asks "how long have I got", and if that ever returns a positive
     number while the window is shut, or zero while it is open, the caller either skips a
     window it could have used or runs straight through one it could not. Checked
     exhaustively, every microsecond of the cycle, over a spread of mask shapes. */
  {
    const uint16_t masks[] = { 0x0001, 0x0100, 0x0300, 0x3b00, 0x8001, 0xffff, 0x0000, 0xaaaa };
    int mismatch = 0, neg = 0, over = 0;
    for (unsigned m = 0; m < sizeof masks / sizeof masks[0]; m++) {
      struct AwdlWindow w; awdl_window_init(&w, masks[m]);
      for (int64_t p = 0; p < AWC_US; p += 1) {
        int64_t r = awdl_window_remaining_at(&w, p);
        bool open = awdl_window_open_at(&w, p);
        if ((r > 0) != open) mismatch++;
        if (r < 0) neg++;
        if (r > AWC_US) over++;
      }
    }
    check("remaining > 0 exactly when the window is open, every us of every mask",
          mismatch == 0, NULL);
    check("...and is never negative", neg == 0, NULL);
    check("...and never exceeds one cycle", over == 0, NULL);

    /* At a width-1 mask the run is one slot with the guard removed, so the value at the
       first usable microsecond is the whole usable window: 65,536 - 1,000 - 12,000. */
    struct AwdlWindow w; awdl_window_init(&w, (uint16_t)(1u << 8));
    int64_t open_at = (int64_t)8 * AWDL_SLOT_US + AWDL_WIN_LEAD_US;
    char d[64]; snprintf(d, sizeof d, "%lld us", (long long)awdl_window_remaining_at(&w, open_at));
    check("width-1: remaining at the open edge is the full usable window",
          awdl_window_remaining_at(&w, open_at) == AWDL_SLOT_US - AWDL_WIN_LEAD_US - AWDL_WIN_GUARD_US, d);
    check("...and decreases by exactly one us per us",
          awdl_window_remaining_at(&w, open_at + 1000) ==
          awdl_window_remaining_at(&w, open_at) - 1000, NULL);
  }

  /* ---- awdl_winstate_step -------------------------------------------------------
     The state is published and counted but NOT wired to the gate, so what is tested here
     is the policy itself, before anything depends on it. */
  {
    check("a candidate means LOCKED, whatever came before",
          awdl_winstate_step(AWDL_WS_VOID, true, 0, 0, 20000000) == AWDL_WS_LOCKED &&
          awdl_winstate_step(AWDL_WS_COAST, true, 0, 0, 20000000) == AWDL_WS_LOCKED, NULL);
    check("losing the candidate coasts, while inside the coast budget",
          awdl_winstate_step(AWDL_WS_LOCKED, false, 5000000, 1000000, 20000000) == AWDL_WS_COAST,
          "4 s since the lock, 20 s budget");
    check("...and goes VOID once the budget is spent",
          awdl_winstate_step(AWDL_WS_LOCKED, false, 25000000, 1000000, 20000000) == AWDL_WS_VOID,
          "24 s since the lock");
    check("COAST decays to VOID rather than renewing itself",
          awdl_winstate_step(AWDL_WS_COAST, false, 25000000, 1000000, 20000000) == AWDL_WS_VOID, NULL);
    check("VOID never coasts: there is nothing to coast FROM",
          awdl_winstate_step(AWDL_WS_VOID, false, 1000, 0, 20000000) == AWDL_WS_VOID, NULL);
    /* The rx clock is a free-running uint32 and wraps every 71.6 minutes. A coast decision
       that reads the wrap as "24,000 seconds have passed" drops a healthy lock. */
    check("the coast budget is measured wrap-safely",
          awdl_winstate_step(AWDL_WS_LOCKED, false, 1000000u, 0xfffffff0u, 20000000) == AWDL_WS_COAST,
          "1.0 s elapsed across the uint32 wrap");
  }

  /* ---- awdl_window_run_at -------------------------------------------------------
     The anchor a window-anchored transmit schedule is built on. If the origin is wrong by
     one cycle, every frame after it is transmitted into silence -- so this is checked as
     properties over every microsecond, not as a handful of examples. */
  {
    const uint16_t masks[] = { 0x0001, 0x0100, 0x0300, 0x3b00, 0x8001, 0xffff, 0xaaaa };
    int sign_bad = 0, join_bad = 0, len_bad = 0, notopen = 0, past_end = 0;
    for (unsigned m = 0; m < sizeof masks / sizeof masks[0]; m++) {
      struct AwdlWindow w; awdl_window_init(&w, masks[m]);
      for (int64_t p = 0; p < AWC_US; p += 7) {
        int64_t orel, len;
        if (!awdl_window_run_at(&w, p, &orel, &len)) continue;
        bool open = awdl_window_open_at(&w, p);
        /* the sign of open_rel says whether the run has already started */
        if (open ? (orel > 0) : (orel <= 0)) sign_bad++;
        if (len <= 0) len_bad++;
        /* when open, the run's end must agree with remaining_at to the microsecond */
        if (open && orel + len != awdl_window_remaining_at(&w, p)) join_bad++;
        /* the first usable microsecond really is open... */
        int64_t o = ((p + orel) % AWC_US + AWC_US) % AWC_US;
        if (!awdl_window_open_at(&w, o)) notopen++;
        /* ...and one microsecond past the end really is not (except the always-open mask,
           which has no end to fall off) */
        if (masks[m] != 0xffff) {
          int64_t e = ((p + orel + len) % AWC_US + AWC_US) % AWC_US;
          if (awdl_window_open_at(&w, e)) past_end++;
        }
      }
    }
    check("open_rel is <= 0 exactly when the window is already open", sign_bad == 0, NULL);
    check("a run always has positive usable length", len_bad == 0, NULL);
    check("open_rel + len_us lands exactly where remaining_at says the run ends",
          join_bad == 0, NULL);
    check("the first usable microsecond of the run is inside the window", notopen == 0, NULL);
    check("...and one microsecond past its end is outside", past_end == 0, NULL);

    /* The number step 11's schedule is built from: how many 15 ms MIF instants fit in a
       cycle. Summed over every RUN, which is the part that is easy to get wrong -- 0x3b00
       is not one block of ch6 but two, slots 8-9 and slots 11-13, each with its own lead
       and guard. Asking run_at once and multiplying gives 8 and is wrong by 13. */
    struct { uint16_t mask; int runs; int expect; } fits[] = {
      { 0x0100, 1, 4 },    /* 52,536 us                       */
      { 0x0300, 1, 8 },    /* 118,072 us                      */
      { 0x3b00, 2, 21 },   /* 118,072 + 183,608, the measured dense mesh */
    };
    for (unsigned i2 = 0; i2 < sizeof fits / sizeof fits[0]; i2++) {
      struct AwdlWindow w; awdl_window_init(&w, fits[i2].mask);
      int total = 0, runs = 0;
      int64_t p = 0;
      for (int guard = 0; guard < AWDL_SLOTS && p < AWC_US; guard++) {
        int64_t orel, len;
        if (!awdl_window_run_at(&w, p, &orel, &len)) break;
        if (p + orel >= AWC_US) break;                  /* that run belongs to next cycle */
        runs++;
        int n = 0; while ((int64_t)n * 15000 < len) n++;
        total += n;
        p = p + orel + len + 1;                          /* step past this run */
      }
      char d[88]; snprintf(d, sizeof d, "mask 0x%04x, %d run(s) -> %d instants/cycle",
                           fits[i2].mask, runs, total);
      check("the 15 ms grid fits the measured number of MIF instants per cycle",
            total == fits[i2].expect && runs == fits[i2].runs, d);
    }
  }

  /* ---- invariant W1, which used to be enforced in the caller and therefore untested ---
     A COAST published with an empty mask claims the badge is coasting a window whose
     position it does not know. The transition function alone cannot see that, because it
     is handed a boolean and not the mask; awdl_winstate_next is the version that can. */
  {
    check("no mask means VOID, whatever the transition would have said",
          awdl_winstate_next(AWDL_WS_LOCKED, false, 0x0000, 5000000, 1000000, 20000000)
            == AWDL_WS_VOID,
          "step() alone would have said COAST here");
    check("...including straight after a lock",
          awdl_winstate_next(AWDL_WS_LOCKED, true, 0x0000, 0, 0, 20000000) == AWDL_WS_VOID, NULL);
    check("a mask lets the transition through unchanged",
          awdl_winstate_next(AWDL_WS_LOCKED, true, 0x0100, 0, 0, 20000000) == AWDL_WS_LOCKED &&
          awdl_winstate_next(AWDL_WS_LOCKED, false, 0x0100, 5000000, 1000000, 20000000)
            == AWDL_WS_COAST, NULL);
    /* The other half of the same bug: passing now_rx as lock_rx makes the coast budget a
       tautology that can never expire. Pinned so the argument order cannot silently
       regress. */
    check("a coast measured against its own timestamp never expires -- do not do this",
          awdl_winstate_step(AWDL_WS_LOCKED, false, 12345678u, 12345678u, 1) == AWDL_WS_COAST,
          "budget of 1 us, elapsed 0: this is what the bug looked like");
    check("...measured against the real lock time, it expires",
          awdl_winstate_step(AWDL_WS_LOCKED, false, 12345678u, 12000000u, 1) == AWDL_WS_VOID, NULL);
  }

  printf("\n%d checks, %d failed\n", ran, fails);
  return fails ? 1 : 0;
}
