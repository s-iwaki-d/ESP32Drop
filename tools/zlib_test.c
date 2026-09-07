/* Host tests for ad_zlib.h -- the SAME emitter the send path will use.
 *
 * Two things are being pinned here.
 *
 * 1. The stream really is zlib. Not "looks like zlib": the byte layout is checked
 *    field by field here, and tools/test-zlib.sh additionally feeds every case to a
 *    REAL zlib (python) through both the one-shot and the streaming API, because the
 *    streaming API is what a receiver actually uses and it is stricter about where a
 *    stream ends.
 *
 * 2. The writer agrees with the reader. dvzip_complete() in awdl_http.h was written
 *    from captured Apple bytes and decides whether an upload is finished; if our
 *    writer and that reader ever disagree, a transfer either hangs waiting for bytes
 *    that will not come or is declared complete while truncated. The captured
 *    receive-side history says which of those hurts: a truncated DvZip stream whose
 *    prefix still inflates was once reported as a successful transfer with a corrupt
 *    image (awdl_http.h:196-199). So the writer is tested against the real reader,
 *    not against a second copy of my own assumptions.
 *
 *   cc -O2 -o /tmp/zlib_test tools/zlib_test.c && /tmp/zlib_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ESP32Drop/src/airdrop/core/ad_zlib.h"
#include "../ESP32Drop/src/airdrop/core/ad_http.h"     /* dvzip_complete -- the reader half of the contract */

static int fails = 0, ran = 0;
static void check(const char *name, int ok, const char *detail) {
  ran++;
  printf("  [%s] %-58s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}

/* Deterministic filler, so a failure is reproducible from the size alone. */
static uint8_t *mkbuf(uint32_t n) {
  uint8_t *b = (uint8_t *)malloc(n ? n : 1);
  for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)(i * 31 + (i >> 8));
  return b;
}

/* The sizes that matter: empty, tiny, and both sides of the 65535 block boundary --
   the only place the emitter changes shape. */
static const uint32_t SIZES[] = { 0, 1, 100, 65534, 65535, 65536, 131070, 200000 };
#define NSIZES (sizeof(SIZES) / sizeof(SIZES[0]))

int main(void) {
  printf("ad_zlib.h -- stored-block zlib + DvZip framing\n\n");

  /* 1. adz_bound is exact, not an over-estimate. A caller sizing a PSRAM buffer from
     it must not be told to allocate more than the emitter writes, and must never be
     told less (the emitter refuses rather than truncating -- test 3). */
  for (unsigned k = 0; k < NSIZES; k++) {
    uint32_t n = SIZES[k];
    uint8_t *in = mkbuf(n);
    uint32_t cap = adz_bound(n);
    uint8_t *out = (uint8_t *)malloc(cap);
    uint32_t m = adz_deflate_stored(out, cap, in, n);
    char d[80]; snprintf(d, sizeof d, "n=%u wrote=%u bound=%u", n, m, cap);
    check("adz_bound is exact", m == cap, d);
    free(in); free(out);
  }

  /* 2. The header and trailer are the bytes zlib checks. 0x7801 % 31 == 0 is the
     validity test a decoder makes on the first two bytes; getting it wrong yields a
     stream that fails at byte 2 with no other symptom. */
  {
    uint32_t n = 100; uint8_t *in = mkbuf(n);
    uint32_t cap = adz_bound(n); uint8_t *out = (uint8_t *)malloc(cap);
    uint32_t m = adz_deflate_stored(out, cap, in, n);
    check("zlib header is 78 01", out[0] == 0x78 && out[1] == 0x01, NULL);
    check("(CMF<<8|FLG) % 31 == 0", ((out[0] << 8) | out[1]) % 31 == 0, NULL);
    check("final block has BFINAL set, BTYPE stored", out[2] == 0x01, NULL);
    uint16_t len  = (uint16_t)(out[3] | (out[4] << 8));
    uint16_t nlen = (uint16_t)(out[5] | (out[6] << 8));
    check("LEN matches the input", len == n, NULL);
    check("NLEN is the one's complement of LEN", nlen == (uint16_t)~len, NULL);
    check("payload is copied verbatim", memcmp(out + 7, in, n) == 0, NULL);
    uint32_t ad = adz_adler32(ADZ_ADLER_INIT, in, n);
    check("adler32 trailer is big-endian",
          out[m-4] == (uint8_t)(ad >> 24) && out[m-3] == (uint8_t)(ad >> 16) &&
          out[m-2] == (uint8_t)(ad >> 8)  && out[m-1] == (uint8_t)ad, NULL);
    free(in); free(out);
  }

  /* 3. A short buffer is REFUSED, never half-written. A partial zlib stream whose
     prefix still inflates is the exact shape of the bug that once reported a corrupt
     image as a successful transfer. */
  {
    uint32_t n = 1000; uint8_t *in = mkbuf(n);
    uint32_t cap = adz_bound(n); uint8_t *out = (uint8_t *)malloc(cap);
    memset(out, 0xAA, cap);
    uint32_t m = adz_deflate_stored(out, cap - 1, in, n);
    int untouched = 1;
    for (uint32_t i = 0; i < cap; i++) if (out[i] != 0xAA) { untouched = 0; break; }
    check("a one-byte-short buffer returns 0", m == 0, NULL);
    check("...and writes nothing at all", untouched, NULL);
    free(in); free(out);
  }

  /* 4. Multi-block streams set BFINAL only on the last block. Getting this wrong
     produces a stream that decodes to a truncated prefix and stops, silently. */
  {
    uint32_t n = 131070; uint8_t *in = mkbuf(n);       /* exactly 2 full blocks */
    uint32_t cap = adz_bound(n); uint8_t *out = (uint8_t *)malloc(cap);
    adz_deflate_stored(out, cap, in, n);
    uint32_t b0 = 2, b1 = 2 + 5 + ADZ_BLOCK_MAX;
    check("first of two blocks has BFINAL clear", out[b0] == 0x00, NULL);
    check("second of two blocks has BFINAL set",  out[b1] == 0x01, NULL);
    check("block 0 LEN is 65535",
          (uint16_t)(out[b0+1] | (out[b0+2] << 8)) == ADZ_BLOCK_MAX, NULL);
    free(in); free(out);
  }

  /* 5. n = 0 still emits one final block. A zero-length file inside a cpio archive
     is legal and must not produce a stream that decodes to "no end of data". */
  {
    uint32_t cap = adz_bound(0); uint8_t *out = (uint8_t *)malloc(cap);
    uint32_t m = adz_deflate_stored(out, cap, (const uint8_t *)"", 0);
    check("n=0 emits header + one final block + adler",
          m == 11 && out[2] == 0x01 && out[3] == 0 && out[4] == 0, NULL);
    check("n=0 adler is the init value 1",
          out[7] == 0 && out[8] == 0 && out[9] == 0 && out[10] == 1, NULL);
    free(out);
  }

  /* 6. adz_adler32 can be accumulated across chunks. A streaming sender computes it
     as it reads from SD or PSRAM, never holding the whole file. */
  {
    uint32_t n = 200000; uint8_t *in = mkbuf(n);
    uint32_t one = adz_adler32(ADZ_ADLER_INIT, in, n);
    uint32_t acc = ADZ_ADLER_INIT;
    for (uint32_t off = 0; off < n; off += 7919)          /* prime stride */
      acc = adz_adler32(acc, in + off, (n - off > 7919) ? 7919 : (n - off));
    check("adler32 accumulates across arbitrary chunk boundaries", acc == one, NULL);
    free(in);
  }

  /* ---- DvZip framing, checked against the READER in awdl_http.h ---- */

  /* 7. One record plus the end record is judged CERTAIN (2) by dvzip_complete.
     Certainty matters: return 1 means the receiver must wait out a 3000ms settle
     before believing the transfer finished (awdl_http.h:237-241). */
  {
    uint32_t n = 5000; uint8_t *in = mkbuf(n);
    uint32_t cap = adz_dvzip_record_bound(n) + 4;
    uint8_t *out = (uint8_t *)malloc(cap);
    uint32_t p = adz_dvzip_record(out, cap, in, n);
    p += adz_dvzip_end(out + p, cap - p);
    check("record + end record reads as CERTAIN", dvzip_complete(out, p) == 2, NULL);
    free(in); free(out);
  }

  /* 8. Without the end record it reads as only MAYBE (1) -- so the end record is
     mandatory for the sender, not cosmetic. This test is why. */
  {
    uint32_t n = 5000; uint8_t *in = mkbuf(n);
    uint32_t cap = adz_dvzip_record_bound(n);
    uint8_t *out = (uint8_t *)malloc(cap);
    uint32_t p = adz_dvzip_record(out, cap, in, n);
    check("a record with no end record reads as boundary-aligned MAYBE",
          dvzip_complete(out, p) == 1, NULL);
    free(in); free(out);
  }

  /* 9. Several records then the end record: still CERTAIN. Apple's own senders emit
     5-6 records for one image (measured: "DVZIP: 5 chunks, 265033 in -> 266752 out"),
     so multi-record is the normal case, not an edge case. */
  {
    uint32_t n = 20000; uint8_t *in = mkbuf(n);
    uint32_t cap = 6 * adz_dvzip_record_bound(n) + 4;
    uint8_t *out = (uint8_t *)malloc(cap);
    uint32_t p = 0;
    for (int i = 0; i < 5; i++) p += adz_dvzip_record(out + p, cap - p, in, n);
    p += adz_dvzip_end(out + p, cap - p);
    check("five records + end record read as CERTAIN", dvzip_complete(out, p) == 2, NULL);
    free(in); free(out);
  }

  /* 10. A truncated record is INCOMPLETE (0), never mistaken for done. This is the
     property that stops a half-received upload being answered 200 OK. */
  {
    uint32_t n = 5000; uint8_t *in = mkbuf(n);
    uint32_t cap = adz_dvzip_record_bound(n);
    uint8_t *out = (uint8_t *)malloc(cap);
    uint32_t p = adz_dvzip_record(out, cap, in, n);
    check("a record cut in half reads as INCOMPLETE",
          dvzip_complete(out, p / 2) == 0, NULL);
    check("a record one byte short reads as INCOMPLETE",
          dvzip_complete(out, p - 1) == 0, NULL);
    free(in); free(out);
  }

  /* 11. The reader sniffs 0x78 as the zlib marker (awdl_http.h:169). Our emitter
     must produce it as the first byte of every chunk or the reader will decide the
     stream is not DvZip at all. */
  {
    uint32_t n = 100; uint8_t *in = mkbuf(n);
    uint32_t cap = adz_dvzip_record_bound(n); uint8_t *out = (uint8_t *)malloc(cap);
    adz_dvzip_record(out, cap, in, n);
    check("chunk payload begins with the 0x78 the reader looks for",
          out[4] == 0x78, NULL);
    free(in); free(out);
  }

  /* ---- Multi-record body: the grain split a large /Upload needs ----
     A single DvZip record larger than the 128 KiB grain reaches macOS whole but is
     rejected at 100%; a real sender frames /Upload as a run of grain records. These
     pin adz_dvzip_body()/_bound() the same way tests 1-11 pin the single-record path.
     The grain is a parameter here so the split itself is exercised at a small size. */

  /* 12. adz_dvzip_body_bound is exact, like adz_bound: the body writes exactly that
     many bytes, so a PSRAM buffer sized from it is neither short nor wasteful. */
  {
    const uint32_t grain = 65536;
    const uint32_t sizes[] = { 0, 1, 65535, 65536, 65537, 200000, 300000 };
    for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
      uint32_t n = sizes[k]; uint8_t *in = mkbuf(n);
      uint32_t cap = adz_dvzip_body_bound(n, grain);
      uint8_t *out = (uint8_t *)malloc(cap);
      uint32_t m = adz_dvzip_body(out, cap, in, n, grain);
      char d[80]; snprintf(d, sizeof d, "n=%u grain=%u wrote=%u bound=%u", n, grain, m, cap);
      check("adz_dvzip_body_bound is exact", m == cap, d);
      free(in); free(out);
    }
  }

  /* 13. The body is cut into ceil(n/grain) records, each its own zlib stream, then the
     end record -- so dvzip_complete judges it CERTAIN and no single record exceeds the
     grain. That last part is the whole fix: sharingd rejects an oversized record. */
  {
    const uint32_t grain = 65536, n = 200000;   /* 4 records: 65536*3 + 3392 */
    uint8_t *in = mkbuf(n);
    uint32_t cap = adz_dvzip_body_bound(n, grain);
    uint8_t *out = (uint8_t *)malloc(cap);
    uint32_t m = adz_dvzip_body(out, cap, in, n, grain);
    uint32_t pos = 0, recs = 0, maxpay = 0; int shape = 1, over = 0;
    while (pos + 4 <= m) {
      uint32_t h = ((uint32_t)out[pos] << 24) | ((uint32_t)out[pos+1] << 16) |
                   ((uint32_t)out[pos+2] << 8) | out[pos+3];
      pos += 4;
      if (h == 0) break;                       /* the end record */
      uint32_t plen = h & 0x7fffffffu;
      if (pos + plen > m) { shape = 0; break; }
      if (out[pos] != 0x78) shape = 0;         /* each record is a fresh zlib stream */
      if (plen > adz_bound(grain)) over = 1;   /* no record carries more than one grain */
      pos += plen; recs++;
      if (plen > maxpay) maxpay = plen;
    }
    char d[96]; snprintf(d, sizeof d, "recs=%u expected=%u maxpayload=%u",
                         recs, (n + grain - 1) / grain, maxpay);
    check("body splits into ceil(n/grain) records", recs == (n + grain - 1) / grain, d);
    check("...each record begins with the 0x78 zlib marker", shape, NULL);
    check("...no record exceeds one grain", !over, NULL);
    check("...and the whole body reads as CERTAIN", dvzip_complete(out, m) == 2, NULL);
    free(in); free(out);
  }

  /* 14. A body that will not fit is refused (returns 0). The caller discards the buffer
     on 0, so -- unlike the single-record emitter -- partial records may already be
     written; what matters is that 0 is returned rather than a short, "complete"-looking
     body. */
  {
    const uint32_t grain = 65536, n = 200000;
    uint8_t *in = mkbuf(n);
    uint32_t cap = adz_dvzip_body_bound(n, grain);
    uint8_t *out = (uint8_t *)malloc(cap);
    uint32_t m = adz_dvzip_body(out, cap - 1, in, n, grain);
    check("a one-byte-short body buffer returns 0", m == 0, NULL);
    free(in); free(out);
  }

  /* 15. grain == 0 means "one record for the whole thing" -- the degenerate case must
     still produce a valid, CERTAIN body rather than loop or divide by zero. */
  {
    uint32_t n = 5000; uint8_t *in = mkbuf(n);
    uint32_t cap = adz_dvzip_body_bound(n, 0);
    uint8_t *out = (uint8_t *)malloc(cap);
    uint32_t m = adz_dvzip_body(out, cap, in, n, 0);
    check("grain=0 emits a single-record CERTAIN body",
          m > 0 && dvzip_complete(out, m) == 2, NULL);
    free(in); free(out);
  }

  printf("\n%d checks, %d failed\n", ran, fails);
  return fails ? 1 : 0;
}
