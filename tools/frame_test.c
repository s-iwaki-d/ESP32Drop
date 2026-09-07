/* Host tests for awdl_frame.h -- the SAME MIF builder the firmware transmits.
 *
 * The frame this builds is the one that makes an Apple device accept us as a peer, and
 * its TLV lengths are hand-written literals: a wrong one does not fail loudly, it
 * shifts every following TLV and the peer silently stops parsing. So this file does two
 * different jobs, and both are needed.
 *
 * 1. GOLDEN VECTORS (testdata/mif_golden.inc). Those bytes were produced by the
 *    PRE-EXTRACTION build_mif, compiled verbatim on the host straight out of git with a
 *    deterministic clock shim -- not retyped, not regenerated from this implementation.
 *    Requiring an exact match is what makes "the extraction changed no bytes" a fact
 *    rather than a claim. The file carries its own inputs alongside its bytes, so a
 *    case's parameters and its expected output cannot drift apart.
 *
 * 2. CHAIN STRUCTURE. Golden vectors alone would freeze a wrong frame just as happily
 *    as a right one, so the chain is also walked the way a peer walks it -- and the length literals are
 *    checked against the content they claim to describe.
 *
 *   cc -O2 -o /tmp/frame_test tools/frame_test.c && /tmp/frame_test
 */
#include <stdio.h>
#include <string.h>
#include "../ESP32Drop/src/awdl/core/awdl_frame.h"

static int fails = 0, ran = 0;
static void check(const char *name, int ok, const char *detail) {
  ran++;
  printf("  [%s] %-58s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}

struct Golden {
  const char *name, *instance;
  uint8_t  master[6];
  uint32_t tsf_us;
  int      have_window;
  uint32_t rx_now_us, phase_us, metric, master_counter;
  uint16_t cur_aw_seq;
  const char *hex;
};
static const struct Golden GOLD[] = {
#include "../testdata/mif_golden.inc"
};
#define NGOLD (sizeof(GOLD) / sizeof(GOLD[0]))

static const uint8_t SELF[6] = {0x2a,0x84,0x85,0x43,0xb3,0x0c};

static size_t unhex(const char *h, uint8_t *out) {
  size_t n = 0;
  for (const char *q = h; q[0] && q[1]; q += 2) {
    int hi = (q[0] <= '9') ? q[0] - '0' : (q[0] | 32) - 'a' + 10;
    int lo = (q[1] <= '9') ? q[1] - '0' : (q[1] | 32) - 'a' + 10;
    out[n++] = (uint8_t)((hi << 4) | lo);
  }
  return n;
}

static void fill(struct AwdlMifParams *m, const struct Golden *g) {
  memset(m, 0, sizeof *m);
  m->src = SELF; m->master = g->master; m->instance = g->instance;
  m->tsf_us = g->tsf_us; m->metric = g->metric;
  m->master_counter = g->master_counter;
  m->have_window = g->have_window != 0;
  m->rx_now_us = g->rx_now_us; m->phase_us = g->phase_us;
  m->cur_aw_seq = g->cur_aw_seq;
}

/* Walk the chain exactly as a peer would: fixed AWDL header, then tag/len/value until
   the buffer ends. Returns 0 on a clean walk, else the offset it broke at. */
static int walk(const uint8_t *b, size_t n, int *n_tlv, int *seen_version,
                int *seen_dps, int *n_svc) {
  size_t p = 40;
  *n_tlv = *seen_version = *seen_dps = *n_svc = 0;
  while (p + 3 <= n) {
    uint8_t tag = b[p];
    uint16_t len = (uint16_t)(b[p+1] | (b[p+2] << 8));
    if (p + 3 + len > n) return (int)p;
    (*n_tlv)++;
    if (tag == TLV_VERSION)          *seen_version = 1;
    if (tag == TLV_DATA_PATH_STATE)  *seen_dps = 1;
    if (tag == TLV_SERVICE_RESPONSE) (*n_svc)++;
    p += 3 + len;
  }
  return (p == n) ? 0 : (int)p;
}

int main(void) {
  printf("awdl_frame.h -- MIF builder\n\n");
  static uint8_t buf[AWDL_MIF_MAX], want[AWDL_MIF_MAX];
  size_t worst = 0;

  /* 1. Byte-for-byte against the pre-extraction implementation. */
  for (unsigned k = 0; k < NGOLD; k++) {
    const struct Golden *g = &GOLD[k];
    size_t wn = unhex(g->hex, want);
    struct AwdlMifParams m; fill(&m, g);
    memset(buf, 0xAA, sizeof buf);
    size_t n = awdl_build_mif(buf, sizeof buf, &m);
    if (n > worst) worst = n;
    char d[96];
    snprintf(d, sizeof d, "%s n=%u ref=%u", g->name, (unsigned)n, (unsigned)wn);
    int same = (n == wn) && memcmp(buf, want, wn) == 0;
    if (!same && n == wn) {
      size_t i = 0; while (i < wn && buf[i] == want[i]) i++;
      snprintf(d, sizeof d, "%s FIRST DIFF at byte %u: got %02x want %02x",
               g->name, (unsigned)i, buf[i], want[i]);
    }
    check("byte-identical to the pre-extraction build_mif", same, d);
  }

  /* 2. The chain parses, and carries what a peer requires to promote us. */
  for (unsigned k = 0; k < NGOLD; k++) {
    const struct Golden *g = &GOLD[k];
    struct AwdlMifParams m; fill(&m, g);
    size_t n = awdl_build_mif(buf, sizeof buf, &m);
    int ntlv, ver, dps, svc;
    int broke = walk(buf, n, &ntlv, &ver, &dps, &svc);
    char d[96];
    snprintf(d, sizeof d, "%s tlv=%d svc=%d", g->name, ntlv, svc);
    check("TLV chain walks cleanly to the last byte", broke == 0, d);
    /* Per OWL's rx.c a peer only becomes connectable once it has seen a MIF AND a
       valid version/devclass; data_path_state carries our AWDL address and the ch6
       social-channel bit. Losing either is silent. */
    check("chain carries version + data_path_state + 3 service_response",
          ver && dps && svc == 3, d);
  }

  /* 3. The header a peer keys on. */
  {
    struct AwdlMifParams m; fill(&m, &GOLD[0]);
    size_t n = awdl_build_mif(buf, sizeof buf, &m);
    check("802.11 action frame, broadcast addr1", n > 40 && buf[0] == 0xd0 &&
          memcmp(buf + 4, "\xff\xff\xff\xff\xff\xff", 6) == 0, NULL);
    check("addr2 is us, addr3 is the AWDL bssid",
          memcmp(buf + 10, SELF, 6) == 0 && memcmp(buf + 16, AWDL_BSSID, 6) == 0, NULL);
    check("Apple vendor action header, subtype 3 = MIF",
          memcmp(buf + 24, APPLE_VENDOR, 4) == 0 && buf[30] == 0x03, NULL);
    check("first TLV is sync_params", buf[40] == TLV_SYNC_PARAMS, NULL);
  }

  /* 4. tsf_us lands in both phy_tx and target_tx, little-endian. Receivers pair their
     rx timestamp with phy_tx, so a wrong byte order here corrupts every peer's phase
     estimate rather than failing. */
  {
    struct AwdlMifParams m; fill(&m, &GOLD[0]); m.tsf_us = 0x11223344;
    awdl_build_mif(buf, sizeof buf, &m);
    check("phy_tx == target_tx == tsf_us, little-endian",
          memcmp(buf + 32, "\x44\x33\x22\x11", 4) == 0 &&
          memcmp(buf + 36, "\x44\x33\x22\x11", 4) == 0, NULL);
  }

  /* 5. We advertise ourselves as a LEAF under the elected root. Claiming to be the
     root made iOS read us as out-of-mesh and refuse to peer, and a peer echoing that
     back is the seed of a self-election. */
  {
    struct AwdlMifParams m; fill(&m, &GOLD[0]);
    size_t n = awdl_build_mif(buf, sizeof buf, &m);
    size_t p = 40; int ok_root = 0, ok_leaf = 0;
    while (p + 3 <= n) {
      uint16_t len = (uint16_t)(buf[p+1] | (buf[p+2] << 8));
      if (buf[p] == TLV_ELECTION_V2 && len >= 28) {
        ok_root = memcmp(buf + p + 3, GOLD[0].master, 6) == 0 &&
                  memcmp(buf + p + 9, GOLD[0].master, 6) == 0;
        ok_leaf = buf[p + 3 + 16] == 0x01;          /* distance_to_master = 1 */
      }
      p += 3 + len;
    }
    check("election_v2 names the elected root as master and sync addr", ok_root, NULL);
    check("election_v2 advertises us at distance 1, never as root", ok_leaf, NULL);
  }

  /* 6. metric 0 falls back to 60, so we never advertise a zero-metric root. */
  {
    struct AwdlMifParams m; fill(&m, &GOLD[0]); m.metric = 0;
    size_t n = awdl_build_mif(buf, sizeof buf, &m);
    int found60 = 0; size_t p = 40;
    while (p + 3 <= n) {
      uint16_t len = (uint16_t)(buf[p+1] | (buf[p+2] << 8));
      if (buf[p] == TLV_ELECTION_PARAMS && len >= 19)
        found60 = (buf[p+3+11] == 60 && buf[p+3+12] == 0);
      p += 3 + len;
    }
    check("metric 0 is published as 60, not 0", found60, NULL);
  }

  /* 7. have_window = false leaves the live timing fields zero rather than writing
     nonsense. The firmware never transmits unsynced, but the builder does not rely on
     its caller's gate. */
  {
    struct AwdlMifParams m; fill(&m, &GOLD[0]);
    m.have_window = false; m.cur_aw_seq = 0xBEEF;
    awdl_build_mif(buf, sizeof buf, &m);
    check("no window -> remaining_aw and next_aw_seq stay 0",
          buf[40+18] == 0 && buf[40+19] == 0 && buf[40+32] == 0 && buf[40+33] == 0, NULL);
  }

  /* 8. remaining_aw_length is clamped to 16 TU = one AW. */
  {
    struct AwdlMifParams m; fill(&m, &GOLD[0]);
    m.have_window = true; m.rx_now_us = 0; m.phase_us = 0;
    awdl_build_mif(buf, sizeof buf, &m);
    uint16_t rem = (uint16_t)(buf[40+18] | (buf[40+19] << 8));
    char d[48]; snprintf(d, sizeof d, "rem_tu=%u", rem);
    check("remaining_aw_length never exceeds 16 TU", rem <= 16, d);
  }

  /* 9. The buffer contract: refuse rather than overrun. */
  {
    struct AwdlMifParams m; fill(&m, &GOLD[0]);
    check("a buffer under AWDL_MIF_MAX is refused",
          awdl_build_mif(buf, AWDL_MIF_MAX - 1, &m) == 0, NULL);
    char d[64];
    snprintf(d, sizeof d, "worst observed %u of %u", (unsigned)worst, AWDL_MIF_MAX);
    check("AWDL_MIF_MAX has real headroom over the worst frame",
          worst > 0 && worst <= AWDL_MIF_MAX - 64, d);
  }

  /* ---------------- parsing ---------------- */

  /* 10. Round trip: what we build is what we read back. This is the cheapest possible
     guard against the encoder and the decoder drifting to different offsets, which is
     the failure the hand-written TLV lengths make easy. */
  for (unsigned k = 0; k < NGOLD; k++) {
    const struct Golden *g = &GOLD[k];
    struct AwdlMifParams m; fill(&m, g);
    size_t n = awdl_build_mif(buf, sizeof buf, &m);
    struct AwdlActionHdr h;
    char d[96]; snprintf(d, sizeof d, "%s", g->name);
    if (!awdl_parse_action(buf, n, &h)) { check("round trip: header parses", 0, d); continue; }
    check("round trip: subtype 3, phy_tx == target_tx == tsf_us",
          h.subtype == AWDL_SUBTYPE_MIF && h.phy_tx == g->tsf_us && h.tgt_tx == g->tsf_us, d);
    check("round trip: addr2 is us, addr3 is the AWDL bssid",
          memcmp(h.src, SELF, 6) == 0 && memcmp(h.bssid, AWDL_BSSID, 6) == 0, d);
    struct AwdlSyncParams sy; struct AwdlElection e1, e2; struct AwdlChanSeq cs;
    int got_sy = 0, got_e1 = 0, got_e2 = 0, got_cs = 0;
    const uint8_t *cur = h.tlv; struct AwdlTlv t;
    while (awdl_tlv_next(&cur, h.tlv_end, &t)) {
      if (awdl_parse_sync(&t, &sy))        got_sy = 1;
      if (awdl_parse_election_v1(&t, &e1)) got_e1 = 1;
      if (awdl_parse_election_v2(&t, &e2)) got_e2 = 1;
      if (awdl_parse_chanseq(&t, &cs))     got_cs = 1;
    }
    check("round trip: sync, both elections and chanseq all decode",
          got_sy && got_e1 && got_e2 && got_cs, d);
    check("round trip: every decoder recovers the elected root we encoded",
          got_sy && got_e1 && got_e2 &&
          memcmp(sy.master, g->master, 6) == 0 &&
          memcmp(e1.master, g->master, 6) == 0 &&
          memcmp(e2.master, g->master, 6) == 0, d);
    uint32_t want_metric = g->metric ? g->metric : 60;
    check("round trip: metric and counter survive the echo",
          got_e1 && got_e2 && e1.metric == want_metric && e2.metric == want_metric &&
          e2.have_counter && e2.counter == g->master_counter, d);
    check("round trip: aw_seq matches what the window said",
          got_sy && sy.aw_seq == (g->have_window ? g->cur_aw_seq : 0), d);
    check("round trip: chanseq is all ch6, so the mask is every slot",
          got_cs && awdl_chanseq_ch6_mask(&cs) == 0xffff, d);
  }

  /* 11. Real on-air frames this code did NOT produce. They come from an older build
     (flags=111611, two bytes longer), which is the point: the decoder has to read
     frames it did not write. */
  {
    static const struct { unsigned n; const char *hex; } CAPTURED[] = {
#include "../testdata/mif_captured.inc"
    };
    for (unsigned k = 0; k < sizeof(CAPTURED)/sizeof(CAPTURED[0]); k++) {
      static uint8_t f[AWDL_MIF_MAX];
      size_t n = unhex(CAPTURED[k].hex, f);
      struct AwdlActionHdr h;
      char d[96]; snprintf(d, sizeof d, "captured #%u n=%u", k, (unsigned)n);
      int ok = (n == CAPTURED[k].n) && awdl_parse_action(f, n, &h);
      check("captured frame: recognised as an AWDL action frame", ok, d);
      if (!ok) continue;
      int ntlv = 0, got_sy = 0, got_cs = 0;
      struct AwdlSyncParams sy; struct AwdlChanSeq cs;
      const uint8_t *cur = h.tlv; struct AwdlTlv t;
      while (awdl_tlv_next(&cur, h.tlv_end, &t)) {
        ntlv++;
        if (awdl_parse_sync(&t, &sy))    got_sy = 1;
        if (awdl_parse_chanseq(&t, &cs)) got_cs = 1;
      }
      snprintf(d, sizeof d, "captured #%u tlv=%d cur==end=%d", k, ntlv, cur == h.tlv_end);
      check("captured frame: chain walks to the last byte", cur == h.tlv_end, d);
      check("captured frame: sync + chanseq decode", got_sy && got_cs, d);
      check("captured frame: it is a MIF and its chanseq is on ch6",
            h.subtype == AWDL_SUBTYPE_MIF && awdl_chanseq_ch6_mask(&cs) != 0, d);
    }
  }

  /* 12. Rejections. The sync guard is the one that did not exist before: handle_sync
     read the master address at val[21..26] with no length check while the three
     handlers beside it all guarded, and whatever it read went into the table the
     election chooses from. */
  {
    uint8_t v[64]; memset(v, 0x5A, sizeof v);
    struct AwdlTlv t; struct AwdlSyncParams sy;
    struct AwdlElection e; struct AwdlChanSeq cs;
    t.tag = TLV_SYNC_PARAMS; t.val = v;
    t.len = AWDL_SYNC_MIN_LEN - 1;
    check("a short sync TLV is REJECTED, not read past", !awdl_parse_sync(&t, &sy), NULL);
    t.len = AWDL_SYNC_MIN_LEN;
    check("a sync TLV of exactly the minimum length is accepted",
          awdl_parse_sync(&t, &sy), NULL);
    t.tag = TLV_ELECTION_PARAMS; t.len = 18;
    check("a short election_v1 is rejected", !awdl_parse_election_v1(&t, &e), NULL);
    t.tag = TLV_ELECTION_V2;     t.len = 27;
    check("a short election_v2 is rejected", !awdl_parse_election_v2(&t, &e), NULL);
    t.tag = TLV_CHAN_SEQ;        t.len = 37;
    check("a short chanseq is rejected", !awdl_parse_chanseq(&t, &cs), NULL);
    /* and each decoder refuses a TLV that is not its own tag */
    t.tag = TLV_VERSION; t.len = 64;
    check("decoders refuse a TLV of the wrong tag",
          !awdl_parse_sync(&t, &sy) && !awdl_parse_election_v1(&t, &e) &&
          !awdl_parse_election_v2(&t, &e) && !awdl_parse_chanseq(&t, &cs), NULL);
  }

  /* 13. A TLV whose length runs past the end of the frame stops the walk rather than
     reading beyond it. Every peer stops at the first bad length; so do we. */
  {
    struct AwdlMifParams m; fill(&m, &GOLD[0]);
    size_t n = awdl_build_mif(buf, sizeof buf, &m);
    buf[40+1] = 0xff; buf[40+2] = 0xff;          /* claim a 65535-byte first TLV */
    struct AwdlActionHdr h;
    const uint8_t *cur; struct AwdlTlv t; int ntlv = 0;
    check("header still parses with a corrupt TLV length",
          awdl_parse_action(buf, n, &h), NULL);
    cur = h.tlv;
    while (awdl_tlv_next(&cur, h.tlv_end, &t)) ntlv++;
    check("an over-long TLV length stops the walk immediately", ntlv == 0, NULL);
  }

  /* 14. Non-AWDL frames are refused before any field is read. */
  {
    struct AwdlMifParams m; fill(&m, &GOLD[0]);
    size_t n = awdl_build_mif(buf, sizeof buf, &m);
    struct AwdlActionHdr h;
    buf[24] = 0x00;                               /* wrong vendor OUI */
    check("a frame without the Apple vendor header is refused",
          !awdl_parse_action(buf, n, &h), NULL);
    buf[24] = APPLE_VENDOR[0];
    buf[0] = 0x08;                                /* a data frame, not an action frame */
    check("a data frame is refused", !awdl_parse_action(buf, n, &h), NULL);
    buf[0] = 0xd0;
    check("a frame shorter than the AWDL header is refused",
          !awdl_parse_action(buf, AWDL_ACTION_HDR_LEN - 1, &h), NULL);
  }

  printf("\n%d checks, %d failed\n", ran, fails);
  return fails ? 1 : 0;
}
