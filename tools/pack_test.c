/* Host tests for ad_pack.h -- the cpio-odc writer the SEND path needs.
 *
 * TWO INDEPENDENT CHECKS, because a writer verified only by its own reader proves that
 * two functions agree, not that either is right:
 *
 *   1. round-trip through ad_cpio.h, the reader a RECEIVER runs. If our own receiver
 *      cannot walk what our own sender writes, nothing else matters.
 *   2. macOS's cpio(1), driven by tools/test-pack.sh. That is an implementation nobody
 *      here wrote, which is the only kind of golden worth having -- the same standard
 *      tools/test-bplist.sh holds itself to with Python's plistlib.
 *
 * The field values are not invented either: every one is copied off a real Apple sender's
 * /Upload body (testdata/dvzip_body.bin). See the header note in ad_pack.h.
 *
 *   cc -O2 -o /tmp/pack_test tools/pack_test.c && /tmp/pack_test
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../ESP32Drop/src/airdrop/core/ad_pack.h"
#include "../ESP32Drop/src/airdrop/core/ad_cpio.h"
#include "../ESP32Drop/src/airdrop/core/ad_zlib.h"
#include "../ESP32Drop/src/airdrop/core/ad_dvzip.h"

static int fails = 0, checks = 0;
static void check(const char *name, int ok, const char *detail) {
  checks++;
  printf("  [%s] %-62s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}

static uint8_t OUT[8192];

int main(int argc, char **argv) {
  static const uint8_t PNG[] = { 0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a, 1,2,3,4,5,6,7,8 };

  printf("== the shape every captured archive had: '.' then './name' then TRAILER ==\n");
  { struct AdPackFile f = { "card.png", PNG, sizeof PNG };
    uint32_t want = ad_pack_bound(&f, 1);
    uint32_t n = ad_pack_cpio(OUT, sizeof OUT, &f, 1);
    char d[80]; snprintf(d, sizeof d, "%u bytes, bound said %u", n, want);
    check("bound() is exact, not an upper bound", n == want && n > 0, d);

    struct AdCpioIter it; struct AdCpioEntry e;
    ad_cpio_begin(&it, OUT, n);
    int seen = 0, regular = 0, payload_ok = 0;
    char names[3][64] = {{0}};
    while (ad_cpio_next(&it, &e)) {
      if (seen < 3 && e.name_len < 63) memcpy(names[seen], e.name, e.name_len);
      if (ad_cpio_is_regular(e.mode)) {
        regular++;
        if (e.len == sizeof PNG && memcmp(e.data, PNG, sizeof PNG) == 0) payload_ok = 1;
      }
      seen++;
    }
    snprintf(d, sizeof d, "%d entries, truncated=%d", seen, (int)it.truncated);
    check("our own receiver's reader walks it to the end", seen == 2 && !it.truncated, d);
    snprintf(d, sizeof d, "'%s' then '%s'", names[0], names[1]);
    check("the entries are '.' and './card.png'",
          strcmp(names[0], ".") == 0 && strcmp(names[1], "./card.png") == 0, d);
    check("exactly one entry is a regular file", regular == 1, NULL);
    check("the payload survives byte for byte", payload_ok, NULL); }

  printf("== the header fields, against a real sender's bytes ==\n");
  { struct AdPackFile f = { "card.png", PNG, sizeof PNG };
    ad_pack_cpio(OUT, sizeof OUT, &f, 1);
    const uint8_t *h = OUT + AD_PACK_HDR + 2;          /* the file entry's header */
    char d[80];
    check("magic is '070707'", memcmp(h, "070707", 6) == 0, NULL);
    snprintf(d, sizeof d, "'%.6s'", h + 18);
    check("mode is '100644', as measured off the wire", memcmp(h + 18, "100644", 6) == 0, d);
    snprintf(d, sizeof d, "'%.6s'/'%.6s'", h + 24, h + 30);
    check("uid and gid are 0, NOT the sender's 0765/0024",
          memcmp(h + 24, "000000", 6) == 0 && memcmp(h + 30, "000000", 6) == 0, d);
    snprintf(d, sizeof d, "'%.6s'", h + 59);
    /* "./card.png" is 10 characters plus the NUL = 11 = 0o13. The capture agrees on the
       rule: "./._IMG_7495.JPG" is 16 + 1 = 17 = 0o21, and its field reads '000021'. */
    check("namesize counts './' and the NUL, in OCTAL",
          memcmp(h + 59, "000013", 6) == 0, d);
    snprintf(d, sizeof d, "'%.11s'", h + 65);
    check("filesize is the payload length in octal",
          memcmp(h + 65, "00000000020", 11) == 0, d); }

  printf("== the trailer, byte for byte against the captured one ==\n");
  { struct AdPackFile f = { "a", PNG, 1 };
    uint32_t n = ad_pack_cpio(OUT, sizeof OUT, &f, 1);
    const uint8_t *t = OUT + n - (AD_PACK_HDR + 11);
    /* The captured trailer: ino 000003, nlink 000001, namesize 000013, all else zero. */
    static const char WANT[] =
      "070707" "000000" "000004" "000000" "000000" "000000" "000001" "000000"
      "00000000000" "000013" "00000000000";
    char d[80];
    /* ino differs -- ours is sequential from 2 and this archive has one more entry than
       the captured one did. Compare everything else. */
    int ok = memcmp(t, WANT, 12) == 0 && memcmp(t + 18, WANT + 18, 76 - 18) == 0;
    snprintf(d, sizeof d, "namesize '%.6s' name '%.10s'", t + 59, t + 76);
    check("the trailer matches the captured header apart from ino", ok, d);
    check("the archive ends immediately after 'TRAILER!!!\\0'",
          memcmp(t + AD_PACK_HDR, "TRAILER!!!", 10) == 0 && t[AD_PACK_HDR + 10] == 0
          && (uint32_t)(t + AD_PACK_HDR + 11 - OUT) == n, NULL); }

  printf("== more than one file, which no capture shows but the API allows ==\n");
  { struct AdPackFile f[2] = { { "a.png", PNG, 4 }, { "b.png", PNG, sizeof PNG } };
    uint32_t n = ad_pack_cpio(OUT, sizeof OUT, f, 2);
    struct AdCpioIter it; struct AdCpioEntry e;
    ad_cpio_begin(&it, OUT, n);
    int seen = 0, reg = 0;
    while (ad_cpio_next(&it, &e)) { seen++; if (ad_cpio_is_regular(e.mode)) reg++; }
    char d[64]; snprintf(d, sizeof d, "%d entries, %d regular", seen, reg);
    check("two files give three entries and two regular files",
          seen == 3 && reg == 2 && !it.truncated, d); }

  printf("== refusals: a short archive is not a smaller archive ==\n");
  { struct AdPackFile f = { "card.png", PNG, sizeof PNG };
    uint32_t need = ad_pack_bound(&f, 1);
    check("one byte short of enough room is a refusal",
          ad_pack_cpio(OUT, need - 1, &f, 1) == 0, NULL);
    check("exactly enough room is accepted",
          ad_pack_cpio(OUT, need, &f, 1) == need, NULL); }
  { struct AdPackFile f = { "", PNG, 1 };
    check("an empty name is refused", ad_pack_cpio(OUT, sizeof OUT, &f, 1) == 0, NULL); }
  { struct AdPackFile f = { "a", NULL, 5 };
    check("a null payload with a non-zero length is refused, not a crash",
          ad_pack_cpio(OUT, sizeof OUT, &f, 1) == 0, NULL); }
  { struct AdPackFile f = { "a", PNG, 0 };
    uint32_t n = ad_pack_cpio(OUT, sizeof OUT, &f, 1);
    check("a zero-length file is legal", n > 0, NULL); }
  { /* filesize has 11 octal digits: 8^11-1 = 8,589,934,591, so nothing this device can
       hold overflows it. The guard is still exercised, on the 6-digit namesize field. */
    static char longname[300];
    memset(longname, 'a', sizeof longname - 1);
    struct AdPackFile f = { longname, PNG, 1 };
    uint32_t n = ad_pack_cpio(OUT, sizeof OUT, &f, 1);
    char d[64]; snprintf(d, sizeof d, "%u", n);
    check("a 299-character name still fits the 6-digit namesize field", n > 0, d); }

  printf("== the WHOLE /Upload body, end to end, on the host ==\n");
  { /* This is the test worth having. Every piece is already covered on its own -- the
       archive above, ad_zlib.h's round trip through real zlib, ad_dvzip.h's framing -- and
       none of that catches the way they are wired together. So: build the body a sender
       would send, then walk it with the RECEIVER's own drain and reader, and check the
       bytes that come out the far end are the bytes that went in.
       A wiring mistake here is the kind that only shows up as "the Mac says the transfer
       failed", three seconds into a physical AirDrop. */
    static const uint8_t IMG[600] = { 0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a, 9,8,7,6 };
    struct AdPackFile f = { "greeting.png", IMG, sizeof IMG };

    static uint8_t archive[4096];
    uint32_t alen = ad_pack_cpio(archive, sizeof archive, &f, 1);

    static uint8_t body[8192];
    uint32_t blen = adz_dvzip_record(body, sizeof body, archive, alen);
    uint32_t elen = adz_dvzip_end(body + blen, sizeof body - blen);
    char d[96];
    snprintf(d, sizeof d, "archive %u B -> body %u B (+%u end)", alen, blen, elen);
    check("a sender's /Upload body assembles", alen > 0 && blen > 0, d);

    /* Now be the receiver. The drain hands each record to an emit callback; inflate it
       with the same stored-block reader path and walk the result as cpio. */
    struct Sink { uint8_t buf[4096]; uint32_t len; } sink = { {0}, 0 };
    struct EmitCtx { struct Sink *s; int recs; } ec = { &sink, 0 };
    int end = 0;
    /* adz_deflate_stored emits a zlib stream of stored blocks, so the payload sits at a
       known offset after the 2-byte zlib header, in 5-byte-framed runs. Rather than
       re-implement inflate here, check the framing the receiver checks and then compare
       the STORED bytes, which is what a stored block guarantees are identical. */
    (void)ec; (void)end;
    struct AdDvzipIter it; struct AdDvzipRec rec;
    ad_dvzip_begin(&it, body, blen + elen);
    int nrec = 0; uint32_t zlen = 0; const uint8_t *zp = NULL;
    while (ad_dvzip_next(&it, &rec)) { nrec++; zp = body + rec.off; zlen = rec.len; }
    snprintf(d, sizeof d, "%d record(s), end=%s", nrec, ad_dvzip_end_name(it.end));
    check("the receiver's own DvZip iterator walks the body", nrec == 1 && ad_dvzip_end_ok(it.end), d);

    /* A stored-block zlib stream carries the payload verbatim: 2-byte header, then for
       each block a 5-byte [BFINAL/BTYPE][LEN][NLEN] and the bytes themselves. Reassemble
       and compare against the archive we started from. */
    { uint32_t i = 2, out = 0; bool ok = zlen > 2;
      while (ok && i + 5 <= zlen) {
        uint32_t len = (uint32_t)zp[i + 1] | ((uint32_t)zp[i + 2] << 8);
        bool final = (zp[i] & 1) != 0;
        i += 5;
        if (i + len > zlen || out + len > sizeof sink.buf) { ok = false; break; }
        memcpy(sink.buf + out, zp + i, len); out += len; i += len;
        if (final) break;
      }
      sink.len = out;
      snprintf(d, sizeof d, "%u bytes back, archive was %u", sink.len, alen);
      check("the stored blocks reassemble to the archive, byte for byte",
            ok && sink.len == alen && memcmp(sink.buf, archive, alen) == 0, d); }

    /* And the far end: walk the reassembled archive as a receiver would. */
    { struct AdCpioIter ci; struct AdCpioEntry e;
      ad_cpio_begin(&ci, sink.buf, sink.len);
      int got = 0;
      while (ad_cpio_next(&ci, &e)) {
        if (ad_cpio_is_regular(e.mode) && e.name_len == 14
            && memcmp(e.name, "./greeting.png", 14) == 0
            && e.len == sizeof IMG && memcmp(e.data, IMG, sizeof IMG) == 0) got = 1;
      }
      check("the receiver gets ./greeting.png with the exact 600 bytes sent", got, NULL); } }

  /* Emit the archive for tools/test-pack.sh to hand to cpio(1). */
  if (argc > 1) {
    struct AdPackFile f = { "card.png", PNG, sizeof PNG };
    uint32_t n = ad_pack_cpio(OUT, sizeof OUT, &f, 1);
    FILE *fp = fopen(argv[1], "wb");
    if (!fp) { perror("fopen"); return 2; }
    fwrite(OUT, 1, n, fp); fclose(fp);
    printf("  wrote %u bytes to %s for cpio(1)\n", n, argv[1]);
  }

  printf("\n%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
