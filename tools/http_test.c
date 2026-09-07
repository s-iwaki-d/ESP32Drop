/* Host tests for awdl_http.h -- the SAME code the firmware compiles.
 *
 * The byte source is scripted in both content AND TIME: each segment says "these
 * bytes become available at virtual time T". Every bug this layer has produced was
 * a timeout interacting with chunked framing, so a harness that cannot express
 * "the body starts arriving 800ms after we answered" cannot reproduce any of them.
 *
 *   cc -O2 -o /tmp/http_test tools/http_test.c && /tmp/http_test
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../ESP32Drop/src/airdrop/core/ad_http.h"

/* ---- scripted source ---------------------------------------------------- */
#define MAXSEG 64
typedef struct { uint32_t at_ms; const unsigned char *p; int n; } Seg;
typedef struct {
  Seg seg[MAXSEG]; int nseg, cur, off;
  uint32_t now_ms;          /* virtual clock */
  bool close_at_end;        /* after the last segment: close, or stay silent forever */
} Script;

static int script_read(struct HttpRdr *r, unsigned char *buf, int len) {
  Script *s = (Script *)r->ctx;
  uint32_t deadline = s->now_ms + r->timeout_ms;
  for (;;) {
    if (s->cur >= s->nseg) {                       /* nothing left to ever arrive */
      if (s->close_at_end) return -1;
      s->now_ms = deadline; return 0;              /* silence until the timeout */
    }
    Seg *g = &s->seg[s->cur];
    if (g->at_ms > deadline) { s->now_ms = deadline; return 0; }   /* timeout first */
    if (g->at_ms > s->now_ms) s->now_ms = g->at_ms;                /* wait for it */
    int avail = g->n - s->off;
    int take = (avail < len) ? avail : len;
    memcpy(buf, g->p + s->off, take);
    s->off += take;
    if (s->off >= g->n) { s->cur++; s->off = 0; }
    return take;
  }
}
static void script_set_timeout(struct HttpRdr *r, uint32_t ms) { r->timeout_ms = ms; }
static void script_add(Script *s, uint32_t at, const void *p, int n) {
  s->seg[s->nseg].at_ms = at; s->seg[s->nseg].p = (const unsigned char *)p;
  s->seg[s->nseg].n = n; s->nseg++;
}

/* ---- sink -------------------------------------------------------------- */
static bool sink_grow(struct HttpSink *sk, uint32_t need) {
  uint32_t nc = sk->cap ? sk->cap : 1024;
  while (nc < need) nc <<= 1;
  unsigned char *np = (unsigned char *)realloc(sk->buf, nc);
  if (!np) return false;
  sk->buf = np; sk->cap = nc; return true;
}
static void sink_init(struct HttpSink *sk, uint32_t max) {
  memset(sk, 0, sizeof(*sk)); sk->max = max; sk->grow = sink_grow;
}

/* Progress observation, for the case below. Records how many times the callback fired and
   whether the length it saw ever went backwards. */
static int  g_prog_calls = 0;
static uint32_t g_prog_last = 0, g_prog_first = 0;
static bool g_prog_monotonic = true;
/* Stands in for a consuming hook that has decided it can store no more. */
static void sink_abort_after_first(struct HttpSink *sk) { sk->max = 0; }

static void sink_progress(struct HttpSink *sk) {
  if (g_prog_calls == 0) g_prog_first = sk->len;
  if (sk->len < g_prog_last) g_prog_monotonic = false;
  g_prog_last = sk->len; g_prog_calls++;
}

/* Consuming sink, mirroring the firmware's streaming decoder: the progress hook
   drains complete records out of buf and compacts the residue to the front. The
   emit here only counts -- decompression is platform code and stays out of these
   tests, exactly as it stays out of the core headers. */
static int g_cons_recs = 0;
static uint32_t g_cons_pay = 0, g_cons_total = 0, g_cons_prev = 0, g_cons_maxres = 0;
static bool cons_emit(void *ctx, const struct AdDvzipRec *rec, const uint8_t *payload) {
  (void)ctx; (void)payload;
  g_cons_recs++; g_cons_pay += rec->len;
  return true;
}
static void cons_progress(struct HttpSink *sk) {
  g_cons_total += sk->len - g_cons_prev;
  int end;
  uint32_t c = ad_dvzip_drain(sk->buf, sk->len, false, cons_emit, NULL, &end);
  if (c) { memmove(sk->buf, sk->buf + c, sk->len - c); sk->len -= c; }
  if (sk->len > g_cons_maxres) g_cons_maxres = sk->len;
  g_cons_prev = sk->len;
}

/* ---- helpers to build DvZip / chunked bodies ---------------------------- */
static int dvzip_record(unsigned char *out, int payload) {
  out[0] = (unsigned char)(payload >> 24); out[1] = (unsigned char)(payload >> 16);
  out[2] = (unsigned char)(payload >> 8);  out[3] = (unsigned char)payload;
  out[4] = 0x78; out[5] = 0x9c;                    /* zlib header */
  for (int i = 6; i < 4 + payload; i++) out[i] = (unsigned char)(i & 0xff);
  return 4 + payload;
}
static int chunk_wrap(unsigned char *out, const unsigned char *p, int n) {
  int k = sprintf((char *)out, "%X\r\n", n);
  memcpy(out + k, p, n); k += n;
  out[k++] = '\r'; out[k++] = '\n';
  return k;
}

static int fails = 0, ran = 0;
static void check(const char *name, bool ok, const char *detail) {
  ran++;
  printf("  [%s] %-56s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}

/* ======================================================================== */
int main(void) {
  printf("== awdl_http.h host tests ==\n");

  /* --- 1. the exact field failure: answer /Ask, then drain with a budget shorter
     than the body's arrival. Must NOT silently continue: the drain has to report
     failure so the caller closes instead of reading the body as a request line. */
  {
    static const char body[] = "1F7BB\r\n";        /* the real chunk header seen */
    Script s; memset(&s, 0, sizeof s);
    script_add(&s, 800, body, (int)strlen(body));   /* body starts 800ms later */
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    r.timeout_ms = 400;                             /* the budget that broke it */
    bool ok = http_drain_body(&r, true, 0);
    char d[120];
    snprintf(d, sizeof d, "drain=%s consumed=%u poisoned=%d timed_out=%d",
             ok ? "true" : "false", r.consumed, r.poisoned, r.timed_out);
    check("400ms drain vs body arriving at 800ms -> drain reports failure", !ok, d);
    /* and the caller must be able to tell it consumed nothing */
    check("  ...and consumed==0 so the cause is nameable", r.consumed == 0, NULL);
  }

  /* --- 2. a partial line at a timeout must poison, not masquerade as complete */
  {
    static const char part[] = "1F7B";              /* no CRLF, then silence */
    Script s; memset(&s, 0, sizeof s);
    script_add(&s, 0, part, (int)strlen(part));
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    r.timeout_ms = 500;
    char ln[40]; int n = hr_line(&r, ln, sizeof ln);
    char d[80]; snprintf(d, sizeof d, "returned=%d poisoned=%d", n, r.poisoned);
    check("terminatorless line poisons the stream", r.poisoned, d);
  }

  /* --- 3. a bad per-chunk CRLF must poison (the free desync detector) */
  {
    static const char bad[] = "4\r\nABCDXX";        /* chunk data then "XX" not CRLF */
    Script s; memset(&s, 0, sizeof s);
    script_add(&s, 0, bad, (int)strlen(bad));
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    r.timeout_ms = 500;
    bool ok = http_drain_body(&r, true, 0);
    char d[80]; snprintf(d, sizeof d, "drain=%d poisoned=%d", ok, r.poisoned);
    check("chunk not followed by CRLF poisons the stream", !ok && r.poisoned, d);
  }

  /* --- 4. a clean chunked body drains and does NOT poison */
  {
    static const char good[] = "4\r\nABCD\r\n0\r\n\r\n";
    Script s; memset(&s, 0, sizeof s);
    script_add(&s, 0, good, (int)strlen(good));
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    r.timeout_ms = 500;
    bool ok = http_drain_body(&r, true, 0);
    char d[80]; snprintf(d, sizeof d, "drain=%d poisoned=%d", ok, r.poisoned);
    check("well-formed chunked body drains cleanly", ok && !r.poisoned, d);
  }

  /* --- 5. request-line validation: the chunk header must be rejected */
  {
    check("\"1F7BB\" is rejected as a request line", !http_reqline_ok("1F7BB"), NULL);
    check("\"POST /Upload HTTP/1.1\" is accepted",
          http_reqline_ok("POST /Upload HTTP/1.1"), NULL);
    check("\"bplist00...\" is rejected", !http_reqline_ok("bplist00\x01\x02"), NULL);
  }

  /* --- 6. /Upload with an explicit DvZip end record: complete with NO waiting */
  {
    static unsigned char rec[512], wire[4096];
    int rn = dvzip_record(rec, 64);
    static const unsigned char endrec[4] = {0,0,0,0};
    int w = 0;
    w += chunk_wrap(wire + w, rec, rn);
    w += chunk_wrap(wire + w, endrec, 4);
    Script s; memset(&s, 0, sizeof s);
    script_add(&s, 0, wire, w);
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    struct HttpSink sk; sink_init(&sk, 1u << 20);
    int rc = http_recv_chunked(&r, &sk, 3000, 3000, script_set_timeout);
    char d[120]; snprintf(d, sizeof d, "rc=%d len=%u virtual_ms=%u", rc, sk.len, s.now_ms);
    check("explicit DvZip end record -> complete immediately",
          rc == HTTP_RECV_OK_END_RECORD && s.now_ms == 0, d);
    free(sk.buf);
  }

  /* --- A HOSTILE CHUNK SIZE. csz is a hex number the sender chooses. Written as
     "sink->len + csz > sink->max" the guard's own addition wraps, so a header of FFFC0000
     passes a max check it should fail, skips the grow, and reaches hr_read with a negative
     length. Nothing is written -- hr_read ignores a negative length -- but the framing
     desynchronises and the transfer dies, on demand, from anywhere in radio range. --- */
  {
    /* The wrap needs len to be large ALREADY: 0xFFFC0000 + len reaches 2^32 only once len
       is at least 0x40000. A fixture that fires the hostile header at len=0 proves nothing
       -- my first attempt at this test passed with the bug still in place. So: fill the
       sink past the wrap point first, exactly as a real sender would have by the time it
       lies about a chunk size. */
    const uint32_t PRE = 0x40000;                 /* 262,144: the smallest len that wraps */
    static unsigned char big[0x40000 + 64];
    memset(big, 'a', PRE);
    static unsigned char wire[0x40000 + 128];
    int w = sprintf((char *)wire, "%X\r\n", (unsigned)PRE);
    memcpy(wire + w, big, PRE); w += (int)PRE;
    w += sprintf((char *)wire + w, "\r\nFFFC0000\r\n");
    Script s; memset(&s, 0, sizeof s);
    script_add(&s, 0, wire, w);
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    struct HttpSink sk; sink_init(&sk, 8u << 20);
    int rc = http_recv_chunked(&r, &sk, 3000, 3000, script_set_timeout);
    char d[140];
    snprintf(d, sizeof d, "rc=%d len=%u (pre=%u, so len+csz wraps to %u)",
             rc, sk.len, PRE, (unsigned)(PRE + 0xFFFC0000u));
    check("a chunk size that wraps the max guard is refused, not honoured",
          rc == HTTP_RECV_OVERFLOW, d);
    check("...and nothing beyond the honest prefix was appended", sk.len == PRE, d);
    free(sk.buf);
  }
  {
    /* max=0 IS THE ABORT SIGNAL. A consuming progress hook uses it to say "stop
       receiving, I can no longer store what arrives" -- and the residue is non-empty when
       it does, by the hold-back rule. So len > max is a REACHABLE state, and a guard
       written as "csz > max - len" underflows there to four billion and lets the transfer
       carry on. That is the abort mechanism of the streaming decoder, silently disabled. */
    char body[128]; int w = 0;
    w += sprintf(body + w, "8\r\n"); for (int i = 0; i < 8; i++) body[w++] = 'p';
    w += sprintf(body + w, "\r\n8\r\n"); for (int i = 0; i < 8; i++) body[w++] = 'q';
    w += sprintf(body + w, "\r\n0\r\n\r\n");
    Script s; memset(&s, 0, sizeof s); script_add(&s, 0, body, w);
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    struct HttpSink sk; sink_init(&sk, 1u << 20);
    sk.progress = sink_abort_after_first;     /* sets max = 0, leaving len = 8 */
    int rc = http_recv_chunked(&r, &sk, 3000, 3000, script_set_timeout);
    char d[120]; snprintf(d, sizeof d, "rc=%d len=%u max=%u", rc, sk.len, sk.max);
    check("max=0 with a non-empty residue stops the receive, it does not underflow",
          rc == HTTP_RECV_OVERFLOW && sk.len == 8, d);
    free(sk.buf);
  }

  {  /* And the boundary either side of it, so the fix cannot be over-tight. */
    char body[64]; int w = 0;
    w += sprintf(body + w, "10\r\n"); for (int i = 0; i < 16; i++) body[w++] = 'z';
    w += sprintf(body + w, "\r\n0\r\n\r\n");
    Script s; memset(&s, 0, sizeof s); script_add(&s, 0, body, w);
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    struct HttpSink sk; sink_init(&sk, 16);            /* max exactly the body size */
    int rc = http_recv_chunked(&r, &sk, 3000, 3000, script_set_timeout);
    char d[120]; snprintf(d, sizeof d, "rc=%d len=%u", rc, sk.len);
    check("a chunk that exactly fills max is still accepted", sk.len == 16, d);
    free(sk.buf);
  }

  /* --- STORED RECORDS AND COMPLETION. Bit 31 of a DvZip header marks a record whose
     payload is literal bytes. dvzip_complete() used to demand a zlib header on every
     payload, so one stored record made it answer "incomplete" for ever -- and the receive
     could then only ever end on the HTTP last-chunk marker or a timeout, never on the
     structural certainty of an end record. Measured: 6 of the 18 records in a 2.35 MB
     photo are stored. --- */
  {
    static unsigned char body[4096];
    int w = 0;
    body[w++]=0; body[w++]=0; body[w++]=0; body[w++]=40;          /* deflated, 40 bytes */
    body[w++]=0x78; body[w++]=0x9c; for (int i=2;i<40;i++) body[w++]=(unsigned char)i;
    body[w++]=0x80; body[w++]=0; body[w++]=0; body[w++]=32;       /* STORED, 32 bytes */
    for (int i=0;i<32;i++) body[w++]=0xa2;                        /* no zlib header */
    check("a stored record does not make a boundary-aligned body 'incomplete'",
          dvzip_complete(body, (uint32_t)w) == 1, NULL);
    body[w++]=0; body[w++]=0; body[w++]=0; body[w++]=0;           /* explicit end record */
    check("...and an end record after one is still structural certainty",
          dvzip_complete(body, (uint32_t)w) == 2, NULL);
    check("...while a stored record longer than the buffer is still incomplete",
          dvzip_complete(body, (uint32_t)(w - 20)) == 0, NULL);
  }

  /* --- PROGRESS. The byte counter a caller publishes used to be assigned once, after the
     whole body had been read: a 73-second transfer showed "0 KB" for 73 seconds, and a
     transfer that never started showed "Receiving" for ever. Progress reported only at the
     end is not progress, it is a result. --- */
  {
    static unsigned char rec[512], wire[8192];
    int rn = dvzip_record(rec, 200);
    static const unsigned char endrec[4] = {0,0,0,0};
    int w = 0;
    for (int i = 0; i < 5; i++) w += chunk_wrap(wire + w, rec, rn);   /* five chunks */
    w += chunk_wrap(wire + w, endrec, 4);
    Script s2; memset(&s2, 0, sizeof s2);
    script_add(&s2, 0, wire, w);
    struct HttpRdr r; http_rdr_init(&r, &s2, script_read);
    struct HttpSink sk; sink_init(&sk, 1u << 20);
    sk.progress = sink_progress;
    g_prog_calls = 0; g_prog_last = 0; g_prog_first = 0; g_prog_monotonic = true;
    int rc = http_recv_chunked(&r, &sk, 3000, 3000, script_set_timeout);
    char d[140];
    snprintf(d, sizeof d, "rc=%d len=%u calls=%d first=%u last=%u",
             rc, sk.len, g_prog_calls, g_prog_first, g_prog_last);
    check("progress fires DURING the body, not once at the end",
          rc == HTTP_RECV_OK_END_RECORD && g_prog_calls >= 5, d);
    check("...and the length it reports only ever grows", g_prog_monotonic, NULL);
    check("...and its first report is not already the total",
          g_prog_first > 0 && g_prog_first < sk.len, d);
    check("...and its last report equals what the caller ends up with",
          g_prog_last == sk.len, NULL);
    free(sk.buf);
  }
  { /* A null progress hook must be legal: it is optional. */
    static unsigned char rec[512], wire[4096];
    int rn = dvzip_record(rec, 64);
    static const unsigned char endrec[4] = {0,0,0,0};
    int w = 0; w += chunk_wrap(wire + w, rec, rn); w += chunk_wrap(wire + w, endrec, 4);
    Script s3; memset(&s3, 0, sizeof s3); script_add(&s3, 0, wire, w);
    struct HttpRdr r; http_rdr_init(&r, &s3, script_read);
    struct HttpSink sk; sink_init(&sk, 1u << 20);      /* progress left NULL */
    int rc = http_recv_chunked(&r, &sk, 3000, 3000, script_set_timeout);
    check("a NULL progress hook is not a crash", rc == HTTP_RECV_OK_END_RECORD, NULL);
    free(sk.buf);
  }

  /* --- 7. THE SILENT-CORRUPTION CASE. Two records; the sender stalls 3500ms at the
     boundary after the first, then sends the second. A settle window shorter than
     the stall declares the body complete on HALF the data -- and a DvZip prefix
     decompresses on its own, so a truncated image would be reported as a success. */
  {
    static unsigned char rec[8192], w1[8192], w2[8192];
    int rn = dvzip_record(rec, 512);
    int n1 = chunk_wrap(w1, rec, rn);
    int n2 = chunk_wrap(w2, rec, rn);
    /* 1200ms settle (the value first shipped) vs a 3500ms stall */
    {
      Script s; memset(&s, 0, sizeof s);
      script_add(&s, 0, w1, n1); script_add(&s, 3500, w2, n2);
      struct HttpRdr r; http_rdr_init(&r, &s, script_read);
      struct HttpSink sk; sink_init(&sk, 1u << 20);
      int rc = http_recv_chunked(&r, &sk, 10000, 1200, script_set_timeout);
      char d[140];
      snprintf(d, sizeof d, "rc=%d len=%u of %d expected -- 1200ms settle vs 3500ms stall",
               rc, sk.len, rn * 2);
      check("1200ms settle TRUNCATES at a 3500ms stall (documents the danger)",
            rc == HTTP_RECV_OK_SILENCE && sk.len == (uint32_t)rn, d);
      free(sk.buf);
    }
    /* 3000ms settle: still truncates at 3500ms. Recorded honestly -- the shipped
       value is not proof against an arbitrarily long stall, only against the ones
       measured (3004ms, 4104ms is ABOVE it: see the note in the summary). */
    {
      Script s; memset(&s, 0, sizeof s);
      script_add(&s, 0, w1, n1); script_add(&s, 2500, w2, n2);
      struct HttpRdr r; http_rdr_init(&r, &s, script_read);
      struct HttpSink sk; sink_init(&sk, 1u << 20);
      int rc = http_recv_chunked(&r, &sk, 10000, 3000, script_set_timeout);
      char d[140];
      snprintf(d, sizeof d, "rc=%d len=%u of %d expected", rc, sk.len, rn * 2);
      check("3000ms settle survives a 2500ms stall and gets the whole body",
            sk.len == (uint32_t)(rn * 2), d);
      free(sk.buf);
    }
  }

  /* --- 8. /Upload where the sender never terminates and never sends more: the
     boundary + silence path is the only way this completes at all. */
  {
    static unsigned char rec[4096], wire[8192];
    int rn = dvzip_record(rec, 256);
    int w = chunk_wrap(wire, rec, rn);
    Script s; memset(&s, 0, sizeof s);
    script_add(&s, 0, wire, w);                     /* then silence forever */
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    struct HttpSink sk; sink_init(&sk, 1u << 20);
    int rc = http_recv_chunked(&r, &sk, 10000, 3000, script_set_timeout);
    char d[120]; snprintf(d, sizeof d, "rc=%d len=%u settled_at=%ums", rc, sk.len, s.now_ms);
    check("no terminator ever -> completes via boundary+silence, not a 10s stall",
          rc == HTTP_RECV_OK_SILENCE && s.now_ms <= 3100, d);
    free(sk.buf);
  }

  /* --- THE CONSUMING SINK. The firmware's streaming DvZip decoder is an HttpSink
     whose progress hook DRAINS complete records out of buf and compacts it, so the
     body is decoded while it arrives and the outcome is known before the response.
     That is legal only while http_recv_chunked re-reads buf/len after every progress
     call (the contract written at HttpSink.progress); these cases pin it, so a future
     refactor that caches len across the hook fails HERE instead of on a badge. The
     decisive property: dvzip_complete judges only the RESIDUE, and the hold-back rule
     in ad_dvzip_drain is what keeps its verdict equal to the whole-body one. --- */
  {
    static unsigned char rec[512], wire[8192];
    int rn = dvzip_record(rec, 200);
    static const unsigned char endrec[4] = {0,0,0,0};
    int w = 0;
    for (int i = 0; i < 5; i++) w += chunk_wrap(wire + w, rec, rn);
    w += chunk_wrap(wire + w, endrec, 4);
    Script s; memset(&s, 0, sizeof s);
    script_add(&s, 0, wire, w);
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    struct HttpSink sk; sink_init(&sk, 1u << 20);
    sk.progress = cons_progress;
    g_cons_recs = 0; g_cons_pay = 0; g_cons_total = 0; g_cons_prev = 0; g_cons_maxres = 0;
    int rc = http_recv_chunked(&r, &sk, 3000, 3000, script_set_timeout);
    char d[160];
    snprintf(d, sizeof d, "rc=%d recs=%d total=%u residue=%u maxres=%u",
             rc, g_cons_recs, g_cons_total, sk.len, g_cons_maxres);
    check("consuming sink: end record still recognised with records drained away",
          rc == HTTP_RECV_OK_END_RECORD && g_cons_recs == 5, d);
    check("...the residue is exactly the end record the probe judged",
          sk.len == 4, d);
    check("...every body byte was seen once (total = 5 records + end record)",
          g_cons_total == (uint32_t)(5 * rn + 4), d);
    check("...and the buffer never held more than one record plus one arriving",
          g_cons_maxres <= (uint32_t)(2 * rn + 4), d);
    free(sk.buf);
  }
  { /* boundary + silence with a consuming sink: the held-back record is what lets
       dvzip_complete answer "boundary-aligned" -- an empty buffer would answer
       "incomplete" and this transfer would stall out as TRUNCATED. */
    static unsigned char rec[4096], wire[8192];
    int rn = dvzip_record(rec, 256);
    int w = chunk_wrap(wire, rec, rn);
    Script s; memset(&s, 0, sizeof s);
    script_add(&s, 0, wire, w);                          /* then silence forever */
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    struct HttpSink sk; sink_init(&sk, 1u << 20);
    sk.progress = cons_progress;
    g_cons_recs = 0; g_cons_pay = 0; g_cons_total = 0; g_cons_prev = 0; g_cons_maxres = 0;
    int rc = http_recv_chunked(&r, &sk, 10000, 3000, script_set_timeout);
    char d[140];
    snprintf(d, sizeof d, "rc=%d recs=%d residue=%u of %d", rc, g_cons_recs, sk.len, rn);
    check("consuming sink: hold-back keeps boundary+silence completion alive",
          rc == HTTP_RECV_OK_SILENCE && g_cons_recs == 0 && sk.len == (uint32_t)rn, d);
    /* the firmware finishes exactly like this: one final drain over the residue */
    int end; uint32_t c = ad_dvzip_drain(sk.buf, sk.len, true, cons_emit, NULL, &end);
    snprintf(d, sizeof d, "consumed=%u recs=%d end=%d", c, g_cons_recs, end);
    check("...and the final drain releases the held record",
          c == (uint32_t)rn && g_cons_recs == 1 && end == AD_DVZIP_END_EXACT, d);
    free(sk.buf);
  }

  /* --- 9. truncated mid-chunk (the peer closes) must not report success */
  {
    static const char cut[] = "10\r\nABCD";         /* says 16 bytes, sends 4, closes */
    Script s; memset(&s, 0, sizeof s); s.close_at_end = true;
    script_add(&s, 0, cut, (int)strlen(cut));
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    struct HttpSink sk; sink_init(&sk, 1u << 20);
    int rc = http_recv_chunked(&r, &sk, 3000, 3000, script_set_timeout);
    char d[120]; snprintf(d, sizeof d, "rc=%d len=%u poisoned=%d", rc, sk.len, r.poisoned);
    check("chunk shorter than its header -> not OK", rc <= 0, d);
  }

  printf("\n%d checks, %d failed\n", ran, fails);
  return fails ? 1 : 0;
}
