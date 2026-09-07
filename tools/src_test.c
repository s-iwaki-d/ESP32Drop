/* Host tests for awdl_src.h -- the per-SENDER residual table.
 *
 * Two things here are worth testing rather than eyeballing. The first is the phase
 * histogram's decay, which is the instrument that settled whether the channel sequence
 * advances one AW per step or four -- if the decay is wrong the shape smears and the
 * answer it gave stops being readable. The second is the variance formula, which is
 * one-pass and therefore subtracts two nearly equal large numbers; case 6 measures how
 * much that actually costs at this project's real sender offsets instead of repeating
 * the textbook warning about it.
 *
 *   cc -O2 -o /tmp/src_test tools/src_test.c -lm && /tmp/src_test
 */
#include <stdio.h>
#include <stdlib.h>
#include "../ESP32Drop/src/awdl/core/awdl_src.h"

static int fails = 0, ran = 0;
static void check(const char *name, int ok, const char *detail) {
  ran++;
  printf("  [%s] %-58s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}

int main(void) {
  printf("== awdl_src.h host tests ==\n");
  struct AwdlSrcTable t;
  uint8_t a[6] = {2,0,0,0,0,1}, b[6] = {2,0,0,0,0,2};

  /* 1. Rows are per sender, and finding one again keeps what it accumulated. Senders sit
     at different TSF epochs -- measured at -180 ms / +14 ms / +275 ms of each other -- so
     merging two of them into one row is what produced 977 rejected samples and 83% lock
     duty back when the table was keyed by the announced master instead. */
  {
    awdl_src_table_init(&t);
    struct AwdlSrc *ra = awdl_src_row(&t, a, 1000);
    awdl_src_add(ra, 10.0);
    struct AwdlSrc *rb = awdl_src_row(&t, b, 1001);
    check("two senders take two rows", ra != rb, NULL);
    check("the same sender returns the same row", awdl_src_row(&t, a, 1002) == ra, NULL);
    check("...keeping its samples", ra->n == 1, NULL);
    check("a new row starts empty and stamped",
          rb->n == 0 && rb->hist_n == 0 && rb->last_ms == 1001 && rb->used, NULL);
  }

  /* 2. Twelve senders fit; the thirteenth evicts the stalest, never a fresher one. */
  {
    awdl_src_table_init(&t);
    struct AwdlSrc *r[AWDL_SRC_ROWS];
    for (int i = 0; i < AWDL_SRC_ROWS; i++) {
      uint8_t m[6] = {2,0,0,0,0,(uint8_t)i};
      r[i] = awdl_src_row(&t, m, 5000u + 10u * i);
    }
    int distinct = 1;
    for (int i = 0; i < AWDL_SRC_ROWS; i++)
      for (int j = i + 1; j < AWDL_SRC_ROWS; j++) if (r[i] == r[j]) distinct = 0;
    check("twelve senders occupy twelve distinct rows", distinct, NULL);
    uint8_t m13[6] = {2,0,0,0,0,99};
    struct AwdlSrc *r13 = awdl_src_row(&t, m13, 9000);
    check("the thirteenth evicts the stalest row", r13 == r[0], NULL);
    check("...and is re-keyed and re-stamped, not left holding the old sender's samples",
          memcmp(r13->mac, m13, 6) == 0 && r13->last_ms == 9000 && r13->n == 0, NULL);
  }

  /* 3. The histogram: one bin per AW, saturating below UINT8_MAX so an increment can
     never wrap a bin back to zero. A wrapped bin reads as "this sender never listens
     here", which is the opposite of the truth and exactly the reading the TX gate would
     act on. */
  {
    awdl_src_table_init(&t);
    struct AwdlSrc *s = awdl_src_row(&t, a, 0);
    for (int i = 0; i < AWDL_SRC_CAP + 400; i++) awdl_src_hist(s, 8);
    check("a hot bin saturates instead of wrapping", s->hist[8] > 0 && s->hist[8] <= AWDL_SRC_CAP,
          NULL);
    check("...and the sample count keeps rising past it", s->hist_n == AWDL_SRC_CAP + 400, NULL);
    check("the bin index is masked into range, so a bad phase cannot corrupt neighbours",
          (AWDL_SRC_BINS & (AWDL_SRC_BINS - 1)) == 0, "BINS is a power of two");
    awdl_src_table_init(&t);
    s = awdl_src_row(&t, a, 0);
    awdl_src_hist(s, 64 + 3);        /* wraps to 3 */
    awdl_src_hist(s, -1);            /* wraps to 63 */
    check("out-of-range bins wrap rather than write out of bounds",
          s->hist[3] == 1 && s->hist[63] == 1, NULL);
  }

  /* 4. Decay. Halving every 512 frames is what makes the histogram show the PRESENT: a
     sender that moves to a different slot should show the move, not a smear across both
     positions accumulated over the whole uptime. */
  {
    awdl_src_table_init(&t);
    struct AwdlSrc *s = awdl_src_row(&t, a, 0);
    for (int i = 0; i < 511; i++) awdl_src_hist(s, 8);
    uint8_t before = s->hist[8];
    check("no decay before the 512th frame", before == AWDL_SRC_CAP, NULL);
    awdl_src_hist(s, 8);             /* the 512th triggers it */
    check("the 512th frame halves every bin", s->hist[8] == AWDL_SRC_CAP / 2, NULL);

    /* A sender that moves: 512 frames in bin 8, then 512 in bin 20. Bin 20 must dominate
       -- if it did not, the instrument could not show a move at all. */
    awdl_src_table_init(&t);
    s = awdl_src_row(&t, a, 0);
    for (int i = 0; i < 512; i++) awdl_src_hist(s, 8);
    for (int i = 0; i < 512; i++) awdl_src_hist(s, 20);
    char d[64]; snprintf(d, sizeof d, "old bin %u, new bin %u", s->hist[8], s->hist[20]);
    check("after a sender moves, the new position dominates the old", s->hist[20] > s->hist[8], d);
  }

  /* 5. Mean and sd on an empty row are zero, not a division by zero. The ESRC line skips
     n==0 rows, but the accessor is public and the next caller may not. */
  {
    awdl_src_table_init(&t);
    struct AwdlSrc *s = awdl_src_row(&t, a, 0);
    check("mean of an empty row is 0", awdl_src_mean(s) == 0.0, NULL);
    check("sd of an empty row is 0, not NaN", awdl_src_sd(s) == 0.0, NULL);
    awdl_src_add(s, 42.0);
    check("a single sample has that mean and zero spread",
          awdl_src_mean(s) == 42.0 && awdl_src_sd(s) == 0.0, NULL);
  }

  /* 6. THE MEASUREMENT. awdl_src_sd() is one-pass, so how wrong is it, really, at the
     offsets this project's senders actually sit at? Compared against a two-pass reference
     over the identical samples. */
  {
    struct { const char *label; double off, spread; int n; double tol; } cases[] = {
      { "typical sender (offset 9us)",              9.0,      15.0,  5000, 1e-9 },
      { "worst real epoch (offset 275ms)",     275000.0,      30.0,  1000, 1e-3 },
      { "half the AW cycle out (offset 524ms)", 524288.0,      30.0,  1000, 1e-2 },
    };
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
      awdl_src_table_init(&t);
      struct AwdlSrc *s = awdl_src_row(&t, a, 0);
      int n = cases[c].n;
      double *v = (double *)malloc(sizeof(double) * (size_t)n);
      unsigned st = 12345;
      for (int i = 0; i < n; i++) {
        st = st * 1103515245u + 12345u;
        double u = ((st >> 16) & 0x7fff) / 32767.0 * 2.0 - 1.0;
        v[i] = cases[c].off + u * cases[c].spread;
        awdl_src_add(s, v[i]);
      }
      double mn = awdl_src_mean(s), ref = 0;
      for (int i = 0; i < n; i++) { double d = v[i] - mn; ref += d * d; }
      ref = sqrt(ref / n);
      double got = awdl_src_sd(s);
      double rel = fabs(got - ref) / ref;
      char d[96];
      snprintf(d, sizeof d, "one-pass %.6f vs two-pass %.6f (rel %.2g)", got, ref, rel);
      check(cases[c].label, rel < cases[c].tol && got == got, d);
      free(v);
    }

    /* ...and the case that DOES break it, so the clamp is shown to be load-bearing rather
       than asserted to be. A spread of 0.006 us does not occur in this system -- the
       innovation is wrapped into +/-AWC/2 before it arrives and real senders scatter by
       tens of microseconds -- but without the clamp this returns NaN, and it would do so
       for the sender furthest out, i.e. the one worth looking at. */
    awdl_src_table_init(&t);
    struct AwdlSrc *s = awdl_src_row(&t, a, 0);
    unsigned st = 12345;
    for (int i = 0; i < 5000; i++) {
      st = st * 1103515245u + 12345u;
      double u = ((st >> 16) & 0x7fff) / 32767.0 * 2.0 - 1.0;
      awdl_src_add(s, 275000.0 + u * 0.01);
    }
    double naive = s->s2 / s->n - (s->s1 / s->n) * (s->s1 / s->n);
    char d[96]; snprintf(d, sizeof d, "unclamped var = %.6g", naive);
    check("the clamp is load-bearing: unclamped, this case goes negative", naive < 0, d);
    check("...and awdl_src_sd() returns 0 rather than NaN",
          awdl_src_sd(s) == 0.0 && awdl_src_sd(s) == awdl_src_sd(s), NULL);
  }

  printf("\n%d checks, %d failed\n", ran, fails);
  return fails ? 1 : 0;
}
