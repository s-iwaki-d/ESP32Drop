/* Host tests for awdl_tick.h -- the window-anchored transmit schedule.
 *
 * The point is not that the arithmetic works. It is that this project has twice now shipped
 * a cadence whose OUTPUT nobody had computed: a 200 ms MIF floor that silently allowed one
 * frame per window once the gate was tightened, and a relative 5 ms tick that simulation
 * showed delivering LESS than the loop() it was meant to replace. So the central test drives
 * this header through a simulated scheduler -- real window schedule, injected wake jitter,
 * injected per-transmit blocking -- and asserts the frames-per-cycle ceiling is hit in every
 * cell of that grid.
 *
 *   cc -O2 -o /tmp/tick_test tools/tick_test.c -lm && /tmp/tick_test
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <string.h>
#include "../ESP32Drop/src/awdl/core/awdl_tick.h"
#include "../ESP32Drop/src/awdl/core/awdl_peer.h"

static int fails = 0, ran = 0;
static void check(const char *name, int ok, const char *detail) {
  ran++;
  printf("  [%s] %-58s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}

/* A scheduler AND a clock.
 *
 * ⚠️ The clock is the part that matters, and getting it wrong made every case in this file
 * blind at once. The first version fed `in.phase_us = now % AWC_US`, for which `now - phase`
 * is bit-constant -- and the tick's run anchor is exactly `now - phase + a per-run
 * constant`. So an anchor bug that re-anchored on every single pass read 100.0% of ceiling,
 * late 0, every run served, in all 36 cells.
 *
 * The firmware's phase comes from awdl_pll_cycle_phase over a PLL that is tracking a master
 * whose clock drifts against ours -- measured p50 13.2 ppm, p90 19.7, max 80.4 -- and that
 * is republished every 50 ms with fresh measurement noise. So that is what this drives, with
 * the real header, and drift is now a dimension of the grid rather than an assumption.
 *
 * It also charges blocking for the DRAIN, per frame against the deadline, the way the
 * firmware's awdl_netif_flush_tx does. The first version charged only for MIF and announce,
 * so every cell ran with a free drain -- and the drain grants sixteen frames after testing
 * room for one.
 */
static unsigned g_rng = 2463534242u;
static double rnd_pm1(void) {
  g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
  return (double)(g_rng % 20001) / 10000.0 - 1.0;
}

static void sim_cycles(uint16_t mask, int cycles, uint32_t jitter_us, uint32_t tx_cost_us,
                       double drift_ppm, int txq_depth, struct AwdlTick *t, bool master_ok) {
  struct AwdlTickCfg cfg = AWDL_TICK_CFG_DEFAULT;
  awdl_tick_init(t, &cfg);
  struct AwdlPll pll; awdl_pll_init(&pll);
  int64_t now = 0, end = (int64_t)cycles * AWC_US, next_pub = 0;
  double pub_phase = 0, pub_freq = 0; uint32_t pub_tlast = 0; bool have = false;
  int q = txq_depth;
  g_rng = 2463534242u;
  while (now < end) {
    if (now >= next_pub) {                 /* the firmware's 50 ms election re-publish */
      next_pub = now + 50000;
      int64_t off = (int64_t)llround(fmod((double)now * (drift_ppm * 1e-6), (double)AWC_US)
                                     + rnd_pm1() * 15.0);
      double e = 0; awdl_pll_feed(&pll, off, (uint32_t)now, &e);
      pub_phase = pll.phase; pub_freq = pll.freq; pub_tlast = pll.tlast; have = true;
    }
    struct AwdlTickIn in;
    in.now_us = now;
    in.have_lock = have;
    in.master_ok = master_ok;
    in.ch6_mask = mask;
    in.phase_us = 0;
    if (have) {
      struct AwdlPll p; awdl_pll_init(&p);
      p.phase = pub_phase; p.freq = pub_freq; p.tlast = pub_tlast;
      in.phase_us = awdl_pll_cycle_phase(&p, (uint32_t)now);
    }
    struct AwdlTickOut out;
    awdl_tick_step(t, &in, &out);
    if (out.do_mif)      now += tx_cost_us;
    if (out.do_announce) now += tx_cost_us;
    for (int k = 0; k < out.drain_max && q > 0; k++) {
      if (now >= out.drain_until_us) break;      /* the executor's per-frame deadline */
      now += tx_cost_us; q--;
    }
    if (q <= 0) q = txq_depth;                   /* lwIP keeps refilling during a transfer */
    int64_t next = out.next_wake_us;
    if (next <= now) next = now + 1;
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    int64_t j = jitter_us ? (int64_t)(g_rng % (jitter_us + 1)) : 0;
    now = ((next + j + 999) / 1000) * 1000;      /* the task's 1 ms tick-grid sleep */
  }
}

int main(void) {
  printf("== awdl_tick.h host tests (window-anchored) ==\n");
  struct AwdlTick t;

  /* 1. THE CEILING, in every cell of jitter x blocking. A schedule anchored to the window
     should deliver every instant the window has room for, regardless of how late the
     scheduler is or how long the radio blocks -- that is the entire difference from the
     relative tick this replaces. */
  {
    struct { uint16_t mask; int per_cycle; } M[] = {
      { 0x0100, 4 }, { 0x0300, 8 }, { 0x3b00, 21 },
    };
    const uint32_t JIT[] = { 0, 200, 1000 };
    const uint32_t COST[] = { 0, 298, 3354, 7073 };
    /* measured on this hardware: p50 13.2 ppm, p90 19.7, max 80.4 */
    const double   DRIFT[] = { 0.0, 13.2, 19.7, 80.4 };
    int cells = 0, bad = 0;
    char worst[96]; worst[0] = 0;
    for (unsigned m = 0; m < 3; m++)
      for (unsigned j = 0; j < 3; j++)
        for (unsigned c = 0; c < 4; c++)
        for (unsigned d = 0; d < 4; d++) {
          sim_cycles(M[m].mask, 100, JIT[j], COST[c], DRIFT[d], 4, &t, true);
          /* whole cycles only, so a fractional final window cannot masquerade as a miss */
          double per_cycle = (double)t.n_mif / 100.0;
          cells++;
          if (per_cycle < M[m].per_cycle - 0.02 || per_cycle > M[m].per_cycle + 0.02) {
            bad++;
            if (!worst[0])
              snprintf(worst, sizeof worst, "mask 0x%04x jit %u cost %u drift %.1f -> %.3f, want %d",
                       M[m].mask, JIT[j], COST[c], DRIFT[d], per_cycle, M[m].per_cycle);
          }
        }
    char d[128];
    snprintf(d, sizeof d, "%d cells%s%s", cells, bad ? ", first bad: " : " all exact",
             bad ? worst : "");
    check("every instant the window has room for is sent, in all 144 cells", bad == 0, d);
  }

  /* 2. The calibration figure, reproduced. The previous header's oracle put a perfect 2 ms
     poll at 3.800 MIF/s on a width-1 mask; an anchored schedule that cannot at least match
     that has no reason to exist. */
  {
    sim_cycles(0x0100, 572, 0, 298, 13.0, 4, &t, true);   /* 572 cycles ~ 600 s */
    double rate = t.n_mif / (572.0 * AWC_US / 1e6);
    char d[72]; snprintf(d, sizeof d, "%.3f MIF/s (loop() oracle: 3.800, ceiling 3.813)", rate);
    check("the anchored schedule meets the ceiling the poll only approached",
          rate > 3.79 && rate < 3.82, d);
  }

  /* 3. Invariant (c): no lock, no master, or no mask means nothing is transmitted at all,
     and the schedule falls back to the election grid. This is the invariant that keeps a
     badge with no idea where the window is off the air. */
  {
    struct AwdlTickCfg cfg = AWDL_TICK_CFG_DEFAULT;
    struct AwdlTickIn in; struct AwdlTickOut out;
    struct { const char *what; bool lock, master; uint16_t mask; } C[] = {
      { "no lock",   false, true,  0x0100 },
      { "no master", true,  false, 0x0100 },
      { "no mask",   true,  true,  0x0000 },
    };
    for (unsigned i = 0; i < 3; i++) {
      awdl_tick_init(&t, &cfg);
      int tx = 0, drain = 0;
      for (int64_t now = 0; now < 5000000; now += 2000) {
        in.now_us = now; in.phase_us = now % AWC_US;
        in.have_lock = C[i].lock; in.master_ok = C[i].master; in.ch6_mask = C[i].mask;
        awdl_tick_step(&t, &in, &out);
        if (out.do_mif || out.do_announce) tx++;
        if (out.drain_max) drain++;
      }
      char d[64]; snprintf(d, sizeof d, "%s: %d tx, %d drains in 5 s", C[i].what, tx, drain);
      check("invariant (c): nothing is transmitted without lock, master and mask",
            tx == 0 && drain == 0, d);
    }
  }

  /* 4. Overrun catch-up. A 250 ms stall must not shift the grid: the instants it covered are
     skipped and COUNTED, and the schedule resumes on its original phase. A cadence that
     re-anchors after a stall turns one late pass into a permanently displaced train, which
     is the failure mode of the relative tick this replaces. */
  {
    struct AwdlTickCfg cfg = AWDL_TICK_CFG_DEFAULT;
    struct AwdlTickIn in; struct AwdlTickOut out;
    awdl_tick_init(&t, &cfg);
    struct AwdlWindow w; awdl_window_init(&w, 0x0100);
    int64_t open0 = (int64_t)8 * AWDL_SLOT_US + AWDL_WIN_LEAD_US;
    /* first pass right at the window edge, then a 250 ms stall, then resume */
    int64_t marks[] = { open0, open0 + 250000, open0 + AWC_US + 1, open0 + AWC_US + 15000 };
    int64_t fired[8]; int nf = 0;
    for (unsigned i = 0; i < 4; i++) {
      in.now_us = marks[i]; in.phase_us = marks[i] % AWC_US;
      in.have_lock = in.master_ok = true; in.ch6_mask = 0x0100;
      awdl_tick_step(&t, &in, &out);
      if (out.do_mif && nf < 8) fired[nf++] = marks[i];
    }
    char d[96];
    snprintf(d, sizeof d, "late=%lu win %lu/%lu served",
             (unsigned long)t.n_mif_late, (unsigned long)t.win_served, (unsigned long)t.win_total);
    check("a stall skips instants and counts them rather than shifting the grid",
          t.n_mif_late > 0 && t.win_served < t.win_total, d);
    /* And it records WHEN, not only how often. The counters above are lifetime totals, so
       a health verdict built on them can be entered and never left -- measured on hardware:
       run=15919/17394 after five hours, still "degraded" while recovering.
       last_bad_us is what lets that verdict say "not any more". */
    check("...and stamps when it happened, so the verdict can later recover",
          t.last_bad_us != INT64_MIN, NULL);
    /* and the next cycle's instants land on the anchor, not 250 ms past it */
    snprintf(d, sizeof d, "next-cycle frame at anchor+%lld us",
             (long long)(fired[nf-1] - (open0 + AWC_US)));
    check("...and the next run's grid is the window's, not the stall's",
          nf >= 2 && (fired[nf-1] - (open0 + AWC_US)) % 15000 <= 1, d);
  }

  /* 5. Anti-burst. A master appearing mid-run must not empty the run's already-passed
     instants onto the air at once. The instants before the pass are never fired
     retroactively. */
  {
    struct AwdlTickCfg cfg = AWDL_TICK_CFG_DEFAULT;
    struct AwdlTickIn in; struct AwdlTickOut out;
    awdl_tick_init(&t, &cfg);
    int64_t open0 = (int64_t)8 * AWDL_SLOT_US + AWDL_WIN_LEAD_US;
    /* the badge sits with no master until 45 ms into the run, then acquires one */
    for (int64_t now = open0; now < open0 + 45000; now += 2000) {
      in.now_us = now; in.phase_us = now % AWC_US;
      in.have_lock = true; in.master_ok = false; in.ch6_mask = 0x0100;
      awdl_tick_step(&t, &in, &out);
    }
    int burst = 0;
    in.now_us = open0 + 45000; in.phase_us = in.now_us % AWC_US;
    in.have_lock = in.master_ok = true; in.ch6_mask = 0x0100;
    awdl_tick_step(&t, &in, &out);
    if (out.do_mif) burst++;
    char d[64]; snprintf(d, sizeof d, "%d frame(s) on the acquiring pass", burst);
    check("a master appearing 45 ms into a run does not fire the three passed instants",
          burst <= 1, d);
  }

  /* 6. The announce. It rides a MIF that is really sent, and its real period is three
     cycles, not its 2.5 s floor -- because MIFs exist only inside the window. */
  {
    sim_cycles(0x0100, 572, 0, 298, 13.0, 4, &t, true);
    double period = (572.0 * AWC_US / 1e6) / (double)t.n_announce;
    char d[80];
    snprintf(d, sizeof d, "%lu in 572 cycles = one per %.3f s (floor %u ms)",
             (unsigned long)t.n_announce, period, (unsigned)2500);
    check("announces ride the window: the real period is 3 cycles, not 2.5 s",
          period > 3.10 && period < 3.20, d);
    check("...and every announce rode a frame that was actually sent",
          t.n_announce <= t.n_mif, NULL);
  }

  /* 7. The drain gate. It must decline when the worst measured injection would not fit
     before the run ends OR before the next MIF instant -- the MIF is what keeps the badge
     visible and must not queue behind sixteen netif frames. */
  {
    struct AwdlTickCfg cfg = AWDL_TICK_CFG_DEFAULT;
    struct AwdlTickIn in; struct AwdlTickOut out;
    awdl_tick_init(&t, &cfg);
    struct AwdlWindow w; awdl_window_init(&w, 0x0100);
    int64_t open0 = (int64_t)8 * AWDL_SLOT_US + AWDL_WIN_LEAD_US;
    int drained_late = 0, drained_early = 0;
    for (int64_t off = 0; off < 52536; off += 137) {
      int64_t now = open0 + off;
      in.now_us = now; in.phase_us = now % AWC_US;
      in.have_lock = in.master_ok = true; in.ch6_mask = 0x0100;
      awdl_tick_step(&t, &in, &out);
      int64_t left = awdl_window_remaining_at(&w, in.phase_us);
      if (out.drain_max && left <= (int64_t)cfg.tx_worst_us) drained_late++;
      if (out.drain_max) drained_early++;
    }
    char d[72]; snprintf(d, sizeof d, "%d admissions, %d with no room", drained_early, drained_late);
    check("the drain is never admitted without room for the worst injection",
          drained_late == 0 && drained_early > 0, d);
    /* ...and outside the window it is never admitted at all */
    awdl_tick_init(&t, &cfg);
    int outside = 0;
    for (int64_t now = 0; now < AWC_US; now += 997) {
      in.now_us = now; in.phase_us = now % AWC_US;
      in.have_lock = in.master_ok = true; in.ch6_mask = 0x0100;
      awdl_tick_step(&t, &in, &out);
      if (out.drain_max && !awdl_window_open_at(&w, in.phase_us)) outside++;
    }
    check("...and never outside the window at all", outside == 0, NULL);
  }

  /* 8. The elect grid is independent of the window and survives everything else. */
  {
    sim_cycles(0x0100, 57, 1000, 7073, 19.7, 16, &t, true);
    check("the schedule never stops waking", t.n_wake > 0, NULL);
    char d[64]; snprintf(d, sizeof d, "win %lu/%lu served, late=%lu",
                         (unsigned long)t.win_served, (unsigned long)t.win_total,
                         (unsigned long)t.n_mif_late);
    check("under worst-case jitter and blocking, every run is still fully served",
          t.win_served == t.win_total && t.n_mif_late == 0, d);
    check("...and a run that never went wrong leaves no stamp at all",
          t.last_bad_us == INT64_MIN, NULL);
  }

  /* 9. THE ROLLBACK MUST ACTUALLY BE A ROLLBACK.
     AWDL_CADENCE_TASK 0 compiles the same body into loop() at priority 1, driven by
     loop()'s own 2 ms pass instead of by the tick's next_wake_us. If that mode has quietly
     stopped working, the escape hatch is a bitrotted #if and the soak that needs it will
     discover that at the worst moment. So it is driven here the way loop() drives it --
     next_wake_us ignored, fixed 2 ms passes -- and held to the same ceiling. */
  {
    struct AwdlTickCfg cfg = AWDL_TICK_CFG_DEFAULT;
    struct { uint16_t mask; int per_cycle; } M[] = { { 0x0100, 4 }, { 0x0300, 8 }, { 0x3b00, 21 } };
    for (unsigned m = 0; m < 3; m++) {
      awdl_tick_init(&t, &cfg);
      struct AwdlTickIn in; struct AwdlTickOut out;
      const int cycles = 100;
      for (int64_t now = 0; now < (int64_t)cycles * AWC_US; now += 2000) {
        in.now_us = now; in.phase_us = now % AWC_US;
        in.have_lock = in.master_ok = true; in.ch6_mask = M[m].mask;
        awdl_tick_step(&t, &in, &out);       /* out.next_wake_us deliberately ignored */
      }
      double per_cycle = (double)t.n_mif / (double)cycles;
      char d[80]; snprintf(d, sizeof d, "mask 0x%04x: %.2f/cycle, want %d, late=%lu",
                           M[m].mask, per_cycle, M[m].per_cycle, (unsigned long)t.n_mif_late);
      check("poll-driven (the AWDL_CADENCE_TASK=0 rollback) still meets the ceiling",
            per_cycle >= M[m].per_cycle - 0.02, d);
    }
  }

  /* 10. A pass that never comes. If the caller stops calling -- a user loop that blocks for
     a second, or a task that has died -- the runs it slept through must be accounted for
     rather than silently forgiven, because win_served/win_total is what the soak alarms on
     and a frozen perfect ratio is exactly what a dead cadence looks like. */
  {
    struct AwdlTickCfg cfg = AWDL_TICK_CFG_DEFAULT;
    struct AwdlTickIn in; struct AwdlTickOut out;
    awdl_tick_init(&t, &cfg);
    int64_t open0 = (int64_t)8 * AWDL_SLOT_US + AWDL_WIN_LEAD_US;
    /* one healthy run, then a 3-cycle gap, then resume */
    for (int64_t off = 0; off < 52536; off += 2000) {
      int64_t now = open0 + off;
      in.now_us = now; in.phase_us = now % AWC_US;
      in.have_lock = in.master_ok = true; in.ch6_mask = 0x0100;
      awdl_tick_step(&t, &in, &out);
    }
    /* one pass past the end of the run, so it closes and can be counted */
    in.now_us = open0 + 60000; in.phase_us = in.now_us % AWC_US;
    in.have_lock = in.master_ok = true; in.ch6_mask = 0x0100;
    awdl_tick_step(&t, &in, &out);
    uint32_t served_before = t.win_served, total_before = t.win_total;
    for (int64_t off = 0; off < 52536; off += 2000) {
      int64_t now = open0 + 4 * AWC_US + off;      /* three cycles skipped entirely */
      in.now_us = now; in.phase_us = now % AWC_US;
      in.have_lock = in.master_ok = true; in.ch6_mask = 0x0100;
      awdl_tick_step(&t, &in, &out);
    }
    char d[80]; snprintf(d, sizeof d, "served %lu, total %lu after skipping 3 cycles",
                         (unsigned long)t.win_served, (unsigned long)t.win_total);
    check("runs the caller slept through are counted, and counted as unserved",
          t.win_total >= total_before + 3 && t.win_served < t.win_total, d);
    check("...while the run that WAS served still counts as served",
          served_before >= 1, NULL);
  }

  /* 11. A RUN THAT GROWS UNDER US. The mesh densifies from one ch6 slot to five during a
     transfer -- measured, and recorded in awdl_window.h -- so a run really does get longer
     while we are standing in it. awdl_window_run_at walks BACKWARD to the run's first slot,
     so appending a slot to the END leaves open_rel bit-identical: the anchor does not move,
     and a run_end captured only at anchor time stays short. Half the instants then get
     refused, and it is SILENT, because the unserved-instant loop is bounded by the same
     stale value -- late=0, run closes served. */
  {
    struct AwdlTickCfg cfg = AWDL_TICK_CFG_DEFAULT;
    struct AwdlTickIn in; struct AwdlTickOut out;
    struct AwdlTick g; awdl_tick_init(&g, &cfg);
    int64_t open0 = (int64_t)8 * AWDL_SLOT_US + AWDL_WIN_LEAD_US;
    int grew = 0;
    for (int64_t off = 0; off < 118072; off += 1000) {
      int64_t now = open0 + off;
      in.now_us = now; in.phase_us = now % AWC_US;
      in.have_lock = in.master_ok = true;
      /* one slot for the first half of the original run, two from then on */
      in.ch6_mask = (off < 30000) ? 0x0100 : 0x0300;
      awdl_tick_step(&g, &in, &out);
      if (out.do_mif) grew++;
    }
    char d[80]; snprintf(d, sizeof d, "%d frames, late=%lu, run=%lu/%lu",
                         grew, (unsigned long)g.n_mif_late,
                         (unsigned long)g.win_served, (unsigned long)g.win_total);
    check("a run that grows mid-flight delivers the instants it grew into", grew == 8, d);

    /* ...and the mirror: a run that SHRINKS must not keep transmitting past its new end. */
    awdl_tick_init(&g, &cfg);
    int past = 0;
    struct AwdlWindow narrow; awdl_window_init(&narrow, 0x0100);
    for (int64_t off = 0; off < 118072; off += 1000) {
      int64_t now = open0 + off;
      in.now_us = now; in.phase_us = now % AWC_US;
      in.have_lock = in.master_ok = true;
      in.ch6_mask = (off < 60000) ? 0x0300 : 0x0100;
      awdl_tick_step(&g, &in, &out);
      if (out.do_mif && !awdl_window_open_at(&narrow, in.phase_us) && off >= 60000) past++;
    }
    snprintf(d, sizeof d, "%d frames past the narrowed end", past);
    check("...and a run that shrinks stops at the new end, not the old one", past == 0, d);
  }

  printf("\n%d checks, %d failed\n", ran, fails);
  return fails ? 1 : 0;
}
