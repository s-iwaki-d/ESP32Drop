/* Host tests for awdl_peer.h -- the mesh-wide window PLL.
 *
 * This loop decides where in the AW cycle the badge thinks it is, and the transmit gate
 * takes that answer at face value. When it has been wrong the badge has transmitted into
 * a slot nobody was listening on -- 8 AW out, 131 ms, 4,125 frames -- and nothing in the
 * firmware noticed; a human did, by seeing the badge leave the AirDrop UI.
 *
 * So the central test is a DIFFERENTIAL one: a verbatim transcription of the loop as it
 * was written inline in handle_sync(), fed the same samples at the same timestamps, and
 * every field of both states compared for exact bitwise equality after each one. Doubles
 * compared with ==, deliberately: the two do identical operations in identical order, so
 * anything less than exact equality is a real difference.
 *
 *   cc -O2 -o /tmp/peer_test tools/peer_test.c -lm && /tmp/peer_test
 */
#include <stdio.h>
#include <string.h>
#include "../ESP32Drop/src/awdl/core/awdl_peer.h"

static int fails = 0, ran = 0;
static void check(const char *name, int ok, const char *detail) {
  ran++;
  printf("  [%s] %-58s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}

/* ---- the ORIGINAL, transcribed from the original firmware's handle_sync() ---------------- */
static double   r_phase, r_freq, r_slow, r_wander, r_resid;
static uint32_t r_tlast, r_n, r_miss;
static bool     r_init;
static uint32_t r_out, r_relock;
static void ref_reset(void) {
  r_phase = r_freq = r_slow = r_wander = r_resid = 0;
  r_tlast = r_n = r_miss = 0; r_init = false; r_out = r_relock = 0;
}
static void ref_feed(int64_t offset, uint32_t rx_ts) {
  if (!r_init) {
    r_phase = (double)offset; r_freq = 0; r_tlast = rx_ts;
    r_slow = (double)offset; r_wander = 0; r_resid = 0; r_n = 1;
    r_init = true;
  } else {
    uint32_t dt = rx_ts - r_tlast;
    if (dt == 0) dt = 1;
    double pred = r_phase + r_freq * (double)dt;
    double e = (double)offset - pred;
    e = fmod(e, (double)AWC_US);
    if (e > AWC_US / 2) e -= AWC_US; else if (e < -AWC_US / 2) e += AWC_US;
    if (fabs(e) < 15000) {
      r_phase = pred + 0.06 * e;
      r_freq += 2.0e-10 * e;
      if (r_freq >  2e-4) r_freq =  2e-4;
      if (r_freq < -2e-4) r_freq = -2e-4;
      r_phase = fmod(r_phase, (double)AWC_US);
      if (r_phase < 0) r_phase += AWC_US;
      double dphi = r_phase - r_slow;
      dphi = fmod(dphi, (double)AWC_US);
      if (dphi > AWC_US / 2) dphi -= AWC_US; else if (dphi < -AWC_US / 2) dphi += AWC_US;
      r_slow += 0.05 * dphi;
      r_wander = 0.95 * r_wander + 0.05 * fabs(dphi);
      r_resid  = 0.95 * r_resid + 0.05 * e * e;
      r_n++;
      r_miss = 0;
    } else {
      r_out++;
      if (++r_miss >= 8) {
        r_phase = (double)offset; r_freq = 0;
        r_slow = (double)offset; r_wander = 0; r_resid = 0;
        r_n = 1; r_miss = 0;
        r_relock++;
      }
    }
    r_tlast = rx_ts;
  }
}
static bool same(const struct AwdlPll *p) {
  return p->phase == r_phase && p->freq == r_freq && p->slow == r_slow &&
         p->wander == r_wander && p->resid == r_resid && p->tlast == r_tlast &&
         p->n == r_n && p->miss == r_miss && p->init == r_init &&
         p->n_out == r_out && p->n_relock == r_relock;
}

/* A deterministic generator -- no rand(), so a failure reproduces exactly. */
static uint32_t rng_s = 0x1234567u;
static uint32_t rng(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5; return rng_s; }

int main(void) {
  printf("awdl_peer.h -- the mesh window PLL\n\n");
  struct AwdlPll p;

  /* 1. A realistic stream: samples every ~1.05 s (one AWC), phase near constant, noise
     of a few tens of microseconds -- the measured 12-15 us regime, widened. */
  {
    awdl_pll_init(&p); ref_reset(); rng_s = 0x1234567u;
    uint32_t ts = 100000; int64_t truth = 300000;
    int diverged = -1;
    for (int i = 0; i < 20000; i++) {
      ts += 1000000 + (rng() % 100000);
      int64_t off = truth + (int64_t)(rng() % 61) - 30;
      double innov;
      awdl_pll_feed(&p, off, ts, &innov);
      ref_feed(off, ts);
      if (!same(&p)) { diverged = i; break; }
    }
    char d[96];
    if (diverged >= 0) snprintf(d, sizeof d, "diverged at sample %d", diverged);
    else snprintf(d, sizeof d, "20,000 samples, every field bitwise identical");
    check("identical to the inline loop over a realistic stream", diverged < 0, d);
  }

  /* 2. A hostile stream: phase jumps, outliers, bursts of them, duplicate timestamps,
     and offsets that straddle the modular boundary. */
  {
    awdl_pll_init(&p); ref_reset(); rng_s = 0x0badc0deu;
    uint32_t ts = 0; int64_t truth = 0;
    int diverged = -1;
    for (int i = 0; i < 50000; i++) {
      uint32_t r = rng();
      if ((r & 0xff) == 0) truth = (int64_t)(rng() % AWC_US);        /* regime change */
      if ((r & 0x1f) == 0) ts += 0;                                  /* same timestamp */
      else ts += 1 + (rng() % 3000000);
      int64_t off = truth + (int64_t)(rng() % 40001) - 20000;        /* half are outliers */
      off = ((off % AWC_US) + AWC_US) % AWC_US;
      double innov;
      awdl_pll_feed(&p, off, ts, &innov);
      ref_feed(off, ts);
      if (!same(&p)) { diverged = i; break; }
    }
    char d[96];
    if (diverged >= 0) snprintf(d, sizeof d, "diverged at sample %d", diverged);
    else snprintf(d, sizeof d, "50,000 samples incl. relocks, wraps, duplicate timestamps");
    check("identical under jumps, outliers and clock wrap", diverged < 0,
          diverged < 0 ? d : d);
    char d2[80]; snprintf(d2, sizeof d2, "%u outliers, %u relocks exercised", p.n_out, p.n_relock);
    check("the hostile stream actually exercised the reject and re-seed paths",
          p.n_out > 100 && p.n_relock > 0, d2);
  }

  /* 3. The 32-bit rx timestamp wraps every ~71 minutes. AWC_US is 2^20 and 2^32 is a
     multiple of it, which is why the modular arithmetic survives that -- pin it. */
  {
    awdl_pll_init(&p); ref_reset();
    uint32_t ts = 0xfffff000u; int64_t truth = 12345;
    int diverged = -1;
    for (int i = 0; i < 4000; i++) {
      ts += 1000000;                                   /* wraps during the run */
      int64_t off = truth + (i % 7) - 3;
      awdl_pll_feed(&p, off, ts, NULL);
      ref_feed(off, ts);
      if (!same(&p)) { diverged = i; break; }
    }
    check("identical across the 32-bit rx timestamp wrap", diverged < 0, NULL);
  }

  /* 4. Seeding and the outcome codes the caller's statistics depend on. */
  {
    awdl_pll_init(&p);
    double innov = -1;
    check("first sample seeds", awdl_pll_feed(&p, 1000, 500, &innov) == AWDL_PLL_SEEDED, NULL);
    check("...and a seed leaves n = 1, not 0", p.n == 1, NULL);
    check("...and reports itself as locked-to-nothing yet", !awdl_pll_locked(&p), NULL);
    check("a close second sample is accepted",
          awdl_pll_feed(&p, 1010, 500 + 1000000, &innov) == AWDL_PLL_ACCEPTED, NULL);
    check("...and hands back the innovation the caller's histograms need",
          innov > 9.0 && innov < 11.0, NULL);
  }

  /* 5. The re-seed. Eight consecutive outliers, and the eighth adopts the new phase --
     not the ninth. A regime change has to be followed, and waiting one more frame is one
     more frame transmitted into the old window. */
  {
    awdl_pll_init(&p);
    awdl_pll_feed(&p, 0, 0, NULL);
    for (int i = 1; i < 30; i++) awdl_pll_feed(&p, 0, (uint32_t)i * 1000000, NULL);
    uint32_t ts = 30000000;
    int last = -1;
    for (int i = 0; i < 8; i++) { ts += 1000000; last = awdl_pll_feed(&p, 500000, ts, NULL); }
    char d[64]; snprintf(d, sizeof d, "n_out=%u n_relock=%u phase=%.0f", p.n_out, p.n_relock, p.phase);
    check("the EIGHTH consecutive outlier re-seeds, not the ninth",
          last == AWDL_PLL_RELOCK && p.n_relock == 1, d);
    check("...and the loop adopts the new phase exactly", p.phase == 500000.0, NULL);
    check("...and counts all eight as rejected samples", p.n_out == 8, d);

    /* seven is not enough */
    awdl_pll_init(&p);
    awdl_pll_feed(&p, 0, 0, NULL);
    for (int i = 1; i < 30; i++) awdl_pll_feed(&p, 0, (uint32_t)i * 1000000, NULL);
    ts = 30000000;
    for (int i = 0; i < 7; i++) { ts += 1000000; awdl_pll_feed(&p, 500000, ts, NULL); }
    check("seven consecutive outliers do NOT re-seed", p.n_relock == 0, NULL);
    /* and one good sample clears the run */
    awdl_pll_feed(&p, (int64_t)awdl_pll_predict(&p, ts + 1000000), ts + 1000000, NULL);
    check("...and a single good sample resets the run to zero", p.miss == 0, NULL);
  }

  /* 6. Lock. It is a statement about the SMOOTHED track, not the last sample. */
  {
    awdl_pll_init(&p);
    uint32_t ts = 0;
    for (int i = 0; i < 200; i++) { ts += 1048576; awdl_pll_feed(&p, 400000 + (i % 5), ts, NULL); }
    char d[72]; snprintf(d, sizeof d, "n=%u wander=%.1fus noise=%.1fus",
                         p.n, p.wander, awdl_pll_noise_rms(&p));
    check("a steady stream reaches lock", awdl_pll_locked(&p), d);
    check("...with the wander well inside the 1,500us threshold", p.wander < 100.0, d);

    awdl_pll_init(&p); ts = 0;
    for (int i = 0; i < 15; i++) { ts += 1048576; awdl_pll_feed(&p, 400000, ts, NULL); }
    check("too few samples is not lock, however steady", !awdl_pll_locked(&p), NULL);
  }

  /* 7. Prediction wraps into the cycle and never leaves it -- the gate divides by the
     slot width and indexes an array with the result. */
  {
    awdl_pll_init(&p);
    awdl_pll_feed(&p, 1000000, 0, NULL);
    int bad = 0;
    for (uint32_t adv = 0; adv < 40000000u; adv += 97777u) {
      double q = awdl_pll_predict(&p, adv);
      if (!(q >= 0.0 && q < (double)AWC_US)) bad++;
    }
    check("predict() always lands inside [0, AWC_US)", bad == 0, NULL);

    /* predict() returns the OFFSET between our clock and the mesh schedule, not a
       position. With no drift that offset does not move. Getting this backwards is easy
       -- it cost this test one wrong assertion before the code was even suspect. */
    p.freq = 0; p.phase = 1000; p.tlast = 0;
    check("predict() with no drift is constant: it is an offset, not a clock",
          awdl_pll_predict(&p, 2000) == 1000.0 &&
          awdl_pll_predict(&p, 99999999u) == 1000.0, NULL);

    /* The POSITION is what the gate consumes, and it advances with the clock. */
    check("cycle_phase advances with the clock when the offset is fixed",
          awdl_pll_cycle_phase(&p, 2000) == 1000 &&
          awdl_pll_cycle_phase(&p, 5000) == 4000, NULL);
    check("cycle_phase wraps at the cycle boundary",
          awdl_pll_cycle_phase(&p, (uint32_t)AWC_US + 1000) == 0, NULL);
    int outside = 0;
    for (uint32_t adv = 0; adv < 40000000u; adv += 88883u) {
      int64_t q = awdl_pll_cycle_phase(&p, adv);
      if (q < 0 || q >= AWC_US) outside++;
    }
    check("cycle_phase always lands inside [0, AWC_US)", outside == 0, NULL);

    /* And it matches what the firmware computed inline, which is the whole point. */
    {
      struct AwdlPll q; awdl_pll_init(&q);
      q.init = true; q.phase = 123456.75; q.freq = 3.5e-6; q.tlast = 0x12345678u;
      int bad = 0;
      for (uint32_t adv = 0; adv < 5000000u; adv += 7919u) {
        uint32_t rx = q.tlast + adv;
        double pred = q.phase + q.freq * (double)(uint32_t)(rx - q.tlast);
        int64_t ref = (((int64_t)rx - (int64_t)pred) % AWC_US + AWC_US) % AWC_US;
        if (awdl_pll_cycle_phase(&q, rx) != ref) bad++;
      }
      check("cycle_phase is identical to the inline computation it replaces", bad == 0, NULL);
    }
  }

  /* 8. The frequency clamp. Drift is measured at 0.1-38 ppm; anything approaching the
     clamp is not drift, and letting the integrator run away moves the window. */
  {
    awdl_pll_init(&p);
    awdl_pll_feed(&p, 0, 0, NULL);
    uint32_t ts = 0;
    for (int i = 0; i < 200000; i++) { ts += 1000; awdl_pll_feed(&p, (int64_t)(i % 14000), ts, NULL); }
    char d[48]; snprintf(d, sizeof d, "freq=%.3g", p.freq);
    check("the drift integrator stays inside its clamp", fabs(p.freq) <= 2e-4, d);
  }

  /* ---- the master table -------------------------------------------------------
     Two jobs: find the row for a master, and choose which row to sacrifice when a ninth
     master appears. The second one is the safety-critical half -- evicting the master we
     are locked to costs the chanseq, and with it the transmit window. */

  /* 9. Find is by address, and a repeat find is the SAME row -- not a fresh one. If it
     ever returned a new row the sample ring would reset on every frame and the election
     would never accumulate the gap statistics it decides on. */
  {
    struct AwdlPeerTable t; awdl_peer_table_init(&t);
    uint8_t a[6] = {2,0,0,0,0,1}, b[6] = {2,0,0,0,0,2};
    struct AwdlPeer *ra = awdl_peer_row(&t, a);
    ra->frames = 77;
    struct AwdlPeer *rb = awdl_peer_row(&t, b);
    check("a second address takes a different row", ra != rb, NULL);
    check("finding the same address again returns the same row",
          awdl_peer_row(&t, a) == ra, NULL);
    check("...and does not wipe what was accumulated in it", ra->frames == 77, NULL);
    check("a new row starts cleared", rb->frames == 0 && rb->n == 0 && !rb->have_chseq, NULL);
    check("a new row is marked used and carries its address",
          rb->used && memcmp(rb->addr, b, 6) == 0, NULL);
  }

  /* 9b. Find-only never allocates. The transmit-side callers ask this inside a critical
     section against the sniffer callback; if asking could create a row, a reader would
     silently be a writer and the locking argument would be wrong. */
  {
    struct AwdlPeerTable t; awdl_peer_table_init(&t);
    uint8_t a[6] = {2,0,0,0,0,1}, miss[6] = {2,0,0,0,0,9};
    check("find on an empty table returns NULL", awdl_peer_find(&t, a) == 0, NULL);
    struct AwdlPeer *ra = awdl_peer_row(&t, a);
    check("find returns the same row awdl_peer_row created", awdl_peer_find(&t, a) == ra, NULL);
    check("find on an unknown address returns NULL", awdl_peer_find(&t, miss) == 0, NULL);
    int used = 0;
    for (int i = 0; i < AWDL_PEERS; i++) if (t.row[i].used) used++;
    check("...and allocated nothing while doing it", used == 1, NULL);
  }

  /* 10. Free slots are taken before anything is evicted. Eight masters must all fit; a
     table that started evicting at, say, six would drop masters in an ordinary room. */
  {
    struct AwdlPeerTable t; awdl_peer_table_init(&t);
    struct AwdlPeer *r[AWDL_PEERS];
    for (int i = 0; i < AWDL_PEERS; i++) {
      uint8_t a[6] = {2,0,0,0,0,(uint8_t)i};
      r[i] = awdl_peer_row(&t, a); r[i]->last_ms = 1000u + 10u * i;
    }
    int distinct = 1;
    for (int i = 0; i < AWDL_PEERS; i++)
      for (int j = i + 1; j < AWDL_PEERS; j++) if (r[i] == r[j]) distinct = 0;
    check("eight masters occupy eight distinct rows, none evicted", distinct, NULL);

    /* 11. The ninth takes the STALEST row -- the smallest last_ms -- and never a fresher
       one. This is the whole point of the eviction rule: ambient Apple devices coming and
       going must not be able to displace the master we are synced to. */
    uint8_t n9[6] = {2,0,0,0,0,99};
    struct AwdlPeer *r9 = awdl_peer_row(&t, n9);
    check("the ninth master evicts the stalest row", r9 == r[0], NULL);
    check("...and the freshest row is untouched",
          r[AWDL_PEERS-1]->last_ms == 1000u + 10u * (AWDL_PEERS-1), NULL);
    check("the evicted row is re-keyed to the new address",
          memcmp(r9->addr, n9, 6) == 0 && r9->last_ms == 0, NULL);
  }

  /* 12. THE KNOWN DEFECT, pinned so the fix has a failing test waiting for it.
     Eviction compares last_ms directly instead of comparing AGE, so it is not safe across
     the millis() wrap at 49.7 days: just after a wrap the FRESHEST row holds the smallest
     value and is chosen first. The firmware has never run that long, and this was left
     exactly as it was rather than quietly changed during an extraction -- but a library
     can be left running for months, so it is written down here rather than in a comment
     nobody runs. Flip the two expectations when the comparison is made wrap-safe. */
  {
    struct AwdlPeerTable t; awdl_peer_table_init(&t);
    for (int i = 0; i < AWDL_PEERS; i++) {
      uint8_t a[6] = {2,0,0,0,0,(uint8_t)i};
      struct AwdlPeer *m = awdl_peer_row(&t, a);
      /* rows 0..6 heard just BEFORE the wrap, row 7 just after: row 7 is the freshest */
      m->last_ms = (i < AWDL_PEERS - 1) ? (0xffffff00u + (uint32_t)i) : 5u;
    }
    uint8_t n9[6] = {2,0,0,0,0,99};
    struct AwdlPeer *r9 = awdl_peer_row(&t, n9);
    check("DEFECT pinned: across the millis() wrap, eviction takes the FRESHEST row",
          r9 == &t.row[AWDL_PEERS-1], "wrap-naive last_ms comparison; fix = compare age");
  }

  /* 12b. The predicate itself, which all three MAC-keyed tables now share. Tested
     directly so the wrap defect is stated once, in the place the fix will be made,
     rather than only as an emergent property of one table's eviction. */
  {
    check("staler() orders two ordinary timestamps", awdl_lru_staler(1000u, 2000u), NULL);
    check("...and is false the other way", !awdl_lru_staler(2000u, 1000u), NULL);
    check("...and false for equal timestamps, so a scan keeps the first row it saw",
          !awdl_lru_staler(1000u, 1000u), NULL);
    check("DEFECT pinned: across the wrap it calls the FRESH timestamp the stale one",
          awdl_lru_staler(5u, 0xffffff00u), "fix = compare age against a common now");
  }

  /* 13. The liveness count the STAT line reports. It is what says "no elected master",
     a state measured at 74 seconds during which macOS aged the badge out entirely -- so
     it has to count by AGE, and unlike eviction it already does. */
  {
    struct AwdlPeerTable t; awdl_peer_table_init(&t);
    check("an empty table has no live masters", awdl_peer_live(&t, 100000u, 5000u) == 0, NULL);
    uint8_t a[6] = {2,0,0,0,0,1}, b[6] = {2,0,0,0,0,2};
    awdl_peer_row(&t, a)->last_ms = 99000u;      /* 1 s ago  */
    awdl_peer_row(&t, b)->last_ms = 90000u;      /* 10 s ago */
    check("only masters heard inside the window count as live",
          awdl_peer_live(&t, 100000u, 5000u) == 1, NULL);
    check("a wider window counts both", awdl_peer_live(&t, 100000u, 20000u) == 2, NULL);
    /* ...and across the wrap it keeps counting, which is exactly what eviction above
       does not do: heard at 0xfffff000, asked at 40, the age is 4,136 us and not 4.29e9. */
    struct AwdlPeerTable w; awdl_peer_table_init(&w);
    awdl_peer_row(&w, a)->last_ms = 0xfffff000u;
    check("the age comparison IS wrap-safe here, unlike eviction",
          awdl_peer_live(&w, 40u, 5000u) == 1, "heard at 0xfffff000, asked at 40");
  }

  printf("\n%d checks, %d failed\n", ran, fails);
  return fails ? 1 : 0;
}
