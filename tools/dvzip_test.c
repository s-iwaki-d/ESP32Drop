/* Host tests for ad_dvzip.h -- the SAME framing code the firmware runs.
 *
 * The framing was an untested blob inside the ESP32 port until a 2.4 MB photo exposed
 * what it could not say. It parsed twelve records of a real macOS body correctly, met a
 * header of 0x80020000, declared the stream cut -- and then RETURNED SUCCESS, handing the
 * caller 1,310,889 bytes of a ~2,354,000-byte archive as if it were whole. The only reason
 * anyone found out was that a cpio walker two layers away happened to notice.
 *
 * So the cases below are mostly about how a walk ENDS. A parser fed by a remote device
 * must be able to tell "the sender finished" from "I stopped", and must never report the
 * second as the first.
 *
 * The record sizes are not invented: a real body measured 2,352,629 bytes of input across
 * 12 records at a 131,072-byte grain (a captured transfer).
 *
 *   cc -O2 -o /tmp/dvzip_test tools/dvzip_test.c && /tmp/dvzip_test
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../ESP32Drop/src/airdrop/core/ad_dvzip.h"
#include "../testdata/dvzip_census.inc"

static int fails = 0, ran = 0;
static void check(const char *name, int ok, const char *detail) {
  ran++;
  printf("  [%s] %-66s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}

static uint8_t BUF[1 << 16];
static uint32_t N;

/* Emit collector for the drain tests. `refuse_at` (when >= 0) makes the collector
   refuse that record, 0-indexed, to test that a refusal loses no bytes. */
static uint32_t g_em_recs, g_em_pay, g_em_stored;
static int g_em_refuse_at = -1;
static void em_reset(void) { g_em_recs = g_em_pay = g_em_stored = 0; g_em_refuse_at = -1; }
static bool em_collect(void *ctx, const struct AdDvzipRec *r, const uint8_t *pl) {
  (void)ctx; (void)pl;
  if (g_em_refuse_at >= 0 && (int)g_em_recs == g_em_refuse_at) return false;
  g_em_recs++; g_em_pay += r->len; if (r->stored) g_em_stored++;
  return true;
}

static void be32(uint32_t v) {
  BUF[N++] = (uint8_t)(v >> 24); BUF[N++] = (uint8_t)(v >> 16);
  BUF[N++] = (uint8_t)(v >> 8);  BUF[N++] = (uint8_t)v;
}
/* A record with a payload that starts like real zlib, so ad_dvzip_looks_like() sees what
   it would see on the wire. */
static void rec(uint32_t n) {
  be32(n);
  if (n >= 2) { BUF[N++] = 0x78; BUF[N++] = 0x9c; }
  for (uint32_t i = (n >= 2 ? 2 : 0); i < n; i++) BUF[N++] = (uint8_t)(i * 31 + 7);
}

/* Walks to the end and reports the shape of the walk. */
static void walk(uint32_t *nrec, uint32_t *payload, int *end, uint32_t *nflagged) {
  struct AdDvzipIter it; struct AdDvzipRec r;
  ad_dvzip_begin(&it, BUF, N);
  *nrec = *payload = *nflagged = 0;
  while (ad_dvzip_next(&it, &r)) {
    (*nrec)++; *payload += r.len; if (ad_dvzip_is_flagged(r.hdr)) (*nflagged)++;
  }
  *end = it.end;
}

int main(void) {
  char d[160];

  /* --- 1. The ordinary case: three records, then an explicit end record. --- */
  N = 0; rec(46); rec(1000); rec(64); be32(0);
  { uint32_t n, pay, fl; int end; walk(&n, &pay, &end, &fl);
    snprintf(d, sizeof d, "records=%u payload=%u end=%s", n, pay, ad_dvzip_end_name(end));
    check("three records + zero record -> END_RECORD, all payload seen",
          n == 3 && pay == 46 + 1000 + 64 && end == AD_DVZIP_END_RECORD, d); }

  /* --- 2. No end record, but the last one lands exactly on the buffer end. This is a
     MAYBE, not a yes, and the enum has to keep the two apart. --- */
  N = 0; rec(46); rec(1000);
  { uint32_t n, pay, fl; int end; walk(&n, &pay, &end, &fl);
    snprintf(d, sizeof d, "records=%u end=%s", n, ad_dvzip_end_name(end));
    check("runs out on a boundary -> END_EXACT, distinct from END_RECORD",
          n == 2 && end == AD_DVZIP_END_EXACT, d);
    check("...and END_EXACT is accepted as a boundary, END_RECORD as certainty",
          ad_dvzip_end_ok(AD_DVZIP_END_EXACT) && ad_dvzip_end_ok(AD_DVZIP_END_RECORD), NULL); }

  /* --- 3. THE FAILURE THIS FILE EXISTS FOR. A header claiming more than remains must be
     a CUT -- a distinct, inspectable state, not a quiet stop that the caller reports as
     success. The numbers are the real ones from the 2.4 MB transfer, scaled to fit. --- */
  N = 0; rec(46); rec(1000); be32(0x80020000u); for (int i = 0; i < 100; i++) BUF[N++] = 0xa5;
  { struct AdDvzipIter it; struct AdDvzipRec r; uint32_t n = 0;
    ad_dvzip_begin(&it, BUF, N);
    while (ad_dvzip_next(&it, &r)) n++;
    snprintf(d, sizeof d, "records=%u end=%s want=%u have=%u pos=%u",
             n, ad_dvzip_end_name(it.end), it.cut_want, it.cut_have, it.pos);
    check("a record longer than the buffer -> END_CUT, not a silent stop",
          n == 2 && it.end == AD_DVZIP_END_CUT, d);
    check("...and END_CUT is NOT a boundary, so a caller cannot report it as complete",
          !ad_dvzip_end_ok(it.end), NULL);
    check("...and the shortfall is reported, which is what separates 'the sender stopped' "
          "from 'we stopped'", it.cut_hdr == 0x80020000u && it.cut_want == 131072u && it.cut_have == 100, d);
    check("...and pos is left ON the header, so the bytes can be dumped for a human",
          it.pos == 4 + 46 + 4 + 1000, d);
    check("...and the header is preserved verbatim, not masked or normalised",
          (((uint32_t)BUF[it.pos] << 24) | ((uint32_t)BUF[it.pos+1] << 16) |
           ((uint32_t)BUF[it.pos+2] << 8) | BUF[it.pos+3]) == 0x80020000u, NULL); }

  /* --- 4. Trailing bytes too few for a header. Silent before: the loop condition just
     went false and nothing said so. --- */
  N = 0; rec(46); BUF[N++] = 1; BUF[N++] = 2; BUF[N++] = 3;
  { uint32_t n, pay, fl; int end; walk(&n, &pay, &end, &fl);
    snprintf(d, sizeof d, "records=%u end=%s", n, ad_dvzip_end_name(end));
    check("1-3 trailing bytes -> END_STRAND, not a clean finish", n == 1 && end == AD_DVZIP_END_STRAND, d);
    check("...and END_STRAND is not a boundary either", !ad_dvzip_end_ok(AD_DVZIP_END_STRAND), NULL); }

  /* --- 5. STORED RECORDS. Bit 31 means the payload is literal bytes, not a zlib stream --
     settled by capture; see the note at the top of ad_dvzip.h. Before that
     this file read the whole 32-bit word as a length, so every stored record demanded
     2 GiB and cut the stream, which is why a 2.4 MB photo arrived as "archive truncated"
     while a small one worked. --- */
  N = 0; be32(0x80000000u | 8); for (int i = 0; i < 8; i++) BUF[N++] = 0xa2; rec(46); be32(0);
  { struct AdDvzipIter it; struct AdDvzipRec r;
    ad_dvzip_begin(&it, BUF, N);
    bool got = ad_dvzip_next(&it, &r);
    snprintf(d, sizeof d, "got=%d hdr=%08x len=%u stored=%d", got, r.hdr, r.len, r.stored);
    check("a stored record is yielded, not treated as a cut", got && r.stored, d);
    check("...and its length is the LOW 31 BITS, not the whole word", r.len == 8, d);
    check("...and the header survives verbatim for anyone who needs to see it",
          r.hdr == (0x80000000u | 8), d);
    got = ad_dvzip_next(&it, &r);
    check("...and the deflated record after it is not marked stored",
          got && !r.stored && r.len == 46, NULL);
    check("...and the walk still finds the end record",
          !ad_dvzip_next(&it, &r) && it.end == AD_DVZIP_END_RECORD, NULL); }
  check("masking an unflagged header changes nothing", ad_dvzip_reclen(46u) == 46u, NULL);
  check("an unflagged header is not reported as flagged", !ad_dvzip_is_flagged(46u), NULL);

  /* --- 5b. THE REAL THING. testdata/dvzip_census.inc is the shape of an actual macOS
     /Upload body -- 18 records carrying a 2.35 MB photo -- with the payloads left out,
     because the payloads are somebody's photograph. Headers and lengths are enough to
     assert everything the framing question turned on. --- */
  {
    static uint8_t big[DVZIP_CENSUS_INLEN];
    uint32_t w = 0;
    for (int i = 0; i < DVZIP_CENSUS_N; i++) {
      uint32_t h = DVZIP_CENSUS[i].hdr;
      big[w++] = (uint8_t)(h >> 24); big[w++] = (uint8_t)(h >> 16);
      big[w++] = (uint8_t)(h >> 8);  big[w++] = (uint8_t)h;
      if (h == 0) break;
      big[w] = (uint8_t)(DVZIP_CENSUS[i].first2 >> 8);       /* 78 or high-entropy, as measured */
      big[w + 1] = (uint8_t)DVZIP_CENSUS[i].first2;
      w += DVZIP_CENSUS[i].len;
    }
    snprintf(d, sizeof d, "built %u of %u bytes", w, DVZIP_CENSUS_INLEN);
    check("the census reproduces the captured body's exact length", w == DVZIP_CENSUS_INLEN, d);

    struct AdDvzipIter it; struct AdDvzipRec r;
    uint32_t n = 0, stored = 0, out = 0, mismatch = 0, zlib_on_stored = 0;
    ad_dvzip_begin(&it, big, DVZIP_CENSUS_INLEN);
    while (ad_dvzip_next(&it, &r)) {
      if (n < (uint32_t)DVZIP_CENSUS_N) {
        if (r.hdr != DVZIP_CENSUS[n].hdr || r.len != DVZIP_CENSUS[n].len) mismatch++;
        if (r.stored != ((DVZIP_CENSUS[n].hdr & AD_DVZIP_FLAG) != 0)) mismatch++;
        /* The property that killed the rival reading: a stored payload carries no zlib
           header. If any did, "flagged but still deflated" would be back on the table. */
        if (r.stored && (DVZIP_CENSUS[n].first2 >> 8) == 0x78) zlib_on_stored++;
        out += DVZIP_CENSUS[n].ulen;
      }
      if (r.stored) stored++;
      n++;
    }
    snprintf(d, sizeof d, "records=%u stored=%u consumed=%u/%u end=%s out=%u",
             n, stored, it.pos, DVZIP_CENSUS_INLEN, ad_dvzip_end_name(it.end), out);
    check("the real body walks to completion: every record, no cut",
          n == (uint32_t)DVZIP_CENSUS_N && it.end == AD_DVZIP_END_EXACT, d);
    check("...landing on exactly the byte the sender stopped at",
          it.pos == DVZIP_CENSUS_CONSUMED && it.pos == DVZIP_CENSUS_INLEN, d);
    check("...with every header and length matching the capture", mismatch == 0, d);
    check("...6 of its 18 records stored, which is why the old parser cut at record 13",
          stored == 6, d);
    check("...no stored payload begins with a zlib header -- 'flagged but still deflated' "
          "stays dead", zlib_on_stored == 0, d);
    check("...and the archive it reconstructs is the measured 2,354,688 bytes",
          out == DVZIP_CENSUS_OUTLEN, d);
  }

  /* --- 6. Shape detection: what tells a DvZip body from a gzip or a bare cpio. --- */
  N = 0; rec(46);
  check("a real first record looks like DvZip", ad_dvzip_looks_like(BUF, N), NULL);
  { const uint8_t gzip[10] = {0x1f, 0x8b, 0x08, 0, 0, 0, 0, 0, 0, 3};
    check("gzip does not", !ad_dvzip_looks_like(gzip, sizeof gzip), NULL); }
  { const uint8_t cpio[10] = {'0','7','0','7','0','7','0','0','0','0'};
    check("a bare odc cpio does not", !ad_dvzip_looks_like(cpio, sizeof cpio), NULL); }
  N = 0; be32(20); BUF[N++] = 0x78; BUF[N++] = 0x9d;      /* CMF/FLG check fails */
  for (int i = 0; i < 18; i++) BUF[N++] = 0;
  check("0x78 with a bad zlib check value does not", !ad_dvzip_looks_like(BUF, N), NULL);
  N = 0; be32(4);
  check("a first record longer than the buffer does not", !ad_dvzip_looks_like(BUF, N), NULL);
  { const uint8_t tiny[4] = {0, 0, 0, 46};
    check("a buffer too short to hold a record does not", !ad_dvzip_looks_like(tiny, 4), NULL); }

  /* --- 7. Degenerate inputs. This walker is driven by a remote sender, so every one of
     these is reachable from the network. --- */
  { struct AdDvzipIter it; struct AdDvzipRec r;
    ad_dvzip_begin(&it, BUF, 0);
    check("an empty buffer ends EXACT and yields nothing",
          !ad_dvzip_next(&it, &r) && it.end == AD_DVZIP_END_EXACT, NULL);
    N = 0; be32(0);
    ad_dvzip_begin(&it, BUF, N);
    check("a lone zero record ends RECORD immediately",
          !ad_dvzip_next(&it, &r) && it.end == AD_DVZIP_END_RECORD, NULL);
    N = 0; be32(0xffffffffu); BUF[N++] = 0x78;
    ad_dvzip_begin(&it, BUF, N);
    check("a 4 GiB-1 header cuts rather than overflowing the position",
          !ad_dvzip_next(&it, &r) && it.end == AD_DVZIP_END_CUT, NULL);
    N = 0; rec(46);
    ad_dvzip_begin(&it, BUF, N);
    while (ad_dvzip_next(&it, &r)) { }
    check("iterating past the end stays finished and yields nothing more",
          !ad_dvzip_next(&it, &r) && it.end == AD_DVZIP_END_EXACT, NULL); }

  /* --- 8. Every record's payload must stay inside the buffer. The one property that,
     if it fails, corrupts memory inside a TLS task instead of returning a bad answer. --- */
  { N = 0; for (int i = 1; i <= 40; i++) rec((uint32_t)(i * 7)); be32(0);
    struct AdDvzipIter it; struct AdDvzipRec r; int bad = 0; uint32_t n = 0;
    ad_dvzip_begin(&it, BUF, N);
    while (ad_dvzip_next(&it, &r)) {
      n++;
      if (r.off > N || r.len > N - r.off) bad++;          /* no overflow: off <= N */
    }
    snprintf(d, sizeof d, "records=%u out-of-bounds=%d", n, bad);
    check("40 records: every payload lies wholly inside the buffer", n == 40 && bad == 0, d); }

  /* --- 9. THE STREAMING DRAIN. ad_dvzip_drain is what lets a receiver decode records
     WHILE the body arrives, so a decode failure lands BEFORE the HTTP response instead
     of after the 200 has already promised success. Its one subtlety is the hold-back
     rule: the completion probe judges the UNCONSUMED bytes, so the drain must leave a
     record to land on and must never consume the zero end record. These cases replay
     the arrival pattern the firmware sees -- bytes appended in arbitrary slices, drained
     after every append, residue compacted to the front. --- */
  { /* 9a. fragmented feed, terminated by an end record: only the end record remains */
    N = 0; rec(46); rec(1000); rec(64); be32(0);
    static uint8_t res[4096]; uint32_t rl2 = 0, fed = 0, maxres = 0;
    em_reset();
    while (fed < N) {
      uint32_t take = (N - fed < 7) ? (N - fed) : 7;          /* 7-byte slices */
      memcpy(res + rl2, BUF + fed, take); rl2 += take; fed += take;
      int end2; uint32_t c = ad_dvzip_drain(res, rl2, false, em_collect, NULL, &end2);
      if (c) { memmove(res, res + c, rl2 - c); rl2 -= c; }
      if (rl2 > maxres) maxres = rl2;
    }
    snprintf(d, sizeof d, "recs=%u pay=%u residue=%u maxres=%u", g_em_recs, g_em_pay, rl2, maxres);
    check("drain over 7-byte slices emits every record exactly once",
          g_em_recs == 3 && g_em_pay == 46 + 1000 + 64, d);
    check("...and the zero end record is never consumed, so the probe can still see it",
          rl2 == 4 && res[0] == 0 && res[1] == 0 && res[2] == 0 && res[3] == 0, d);
  }
  { /* 9b. the hold-back rule: a complete record with nothing after it stays, so a
       boundary-aligned stream still has a boundary for dvzip_complete to land on */
    N = 0; rec(46); rec(1000);
    em_reset();
    int end2; uint32_t c = ad_dvzip_drain(BUF, N, false, em_collect, NULL, &end2);
    snprintf(d, sizeof d, "consumed=%u recs=%u end=%s", c, g_em_recs, ad_dvzip_end_name(end2));
    check("mid-stream, the last complete record is held back, walk reports RUNNING",
          c == 4 + 46 && g_em_recs == 1 && end2 == AD_DVZIP_END_RUNNING, d);
    c = ad_dvzip_drain(BUF + (4 + 46), N - (4 + 46), true, em_collect, NULL, &end2);
    snprintf(d, sizeof d, "consumed=%u recs=%u end=%s", c, g_em_recs, ad_dvzip_end_name(end2));
    check("...and a final drain releases it and lands EXACT",
          c == 4 + 1000 && g_em_recs == 2 && end2 == AD_DVZIP_END_EXACT, d);
  }
  { /* 9c. an emit refusal consumes nothing of the refused record: no bytes are lost */
    N = 0; rec(46); rec(1000); rec(64); be32(0);
    em_reset(); g_em_refuse_at = 1;                     /* refuse the second record */
    int end2; uint32_t c = ad_dvzip_drain(BUF, N, true, em_collect, NULL, &end2);
    snprintf(d, sizeof d, "consumed=%u recs=%u end=%s", c, g_em_recs, ad_dvzip_end_name(end2));
    check("a refused record is not consumed and stops the walk where it stands",
          c == 4 + 46 && g_em_recs == 1 && end2 == AD_DVZIP_END_RUNNING, d);
    check("...leaving the refused record's own header at the front of the residue",
          BUF[c] == 0 && BUF[c + 1] == 0 && BUF[c + 2] == 0x03 && BUF[c + 3] == 0xe8, NULL);
  }
  { /* 9d. THE REAL BODY, ARRIVING AS IT REALLY DOES: the census stream fed in odd
       slices, drained as it grows. The residue must stay bounded by one held record
       plus one arriving record -- the property that lets the firmware hold ~7 MB of
       decoded archive instead of compressed + decoded at once. */
    static uint8_t big[DVZIP_CENSUS_INLEN];
    uint32_t w = 0;
    for (int i = 0; i < DVZIP_CENSUS_N; i++) {
      uint32_t h = DVZIP_CENSUS[i].hdr;
      big[w++] = (uint8_t)(h >> 24); big[w++] = (uint8_t)(h >> 16);
      big[w++] = (uint8_t)(h >> 8);  big[w++] = (uint8_t)h;
      if (h == 0) break;
      big[w] = (uint8_t)(DVZIP_CENSUS[i].first2 >> 8);
      big[w + 1] = (uint8_t)DVZIP_CENSUS[i].first2;
      w += DVZIP_CENSUS[i].len;
    }
    static uint8_t res[1 << 19];                        /* 512 KiB, the firmware's scale */
    uint32_t rl2 = 0, fed = 0, maxres = 0;
    em_reset();
    while (fed < w) {
      uint32_t take = (w - fed < 1013) ? (w - fed) : 1013;
      memcpy(res + rl2, big + fed, take); rl2 += take; fed += take;
      int end2; uint32_t c = ad_dvzip_drain(res, rl2, false, em_collect, NULL, &end2);
      if (c) { memmove(res, res + c, rl2 - c); rl2 -= c; }
      if (rl2 > maxres) maxres = rl2;
    }
    int end2; uint32_t c = ad_dvzip_drain(res, rl2, true, em_collect, NULL, &end2);
    memmove(res, res + c, rl2 - c); rl2 -= c;
    snprintf(d, sizeof d, "recs=%u stored=%u pay=%u maxres=%u residue=%u end=%s",
             g_em_recs, g_em_stored, g_em_pay, maxres, rl2, ad_dvzip_end_name(end2));
    check("the census body drains to completion: 18 records, 6 stored, nothing left",
          g_em_recs == 18 && g_em_stored == 6 && rl2 == 0 && end2 == AD_DVZIP_END_EXACT, d);
    check("...every payload byte accounted for (input minus 18 headers)",
          g_em_pay == DVZIP_CENSUS_CONSUMED - 4u * 18u, d);
    check("...and the residue never exceeded one held record plus one arriving record",
          maxres <= 131118u + 4u + 131118u + 4u + 1013u, d);
  }

  printf("\n%d checks, %d failed\n", ran, fails);
  return fails ? 1 : 0;
}
