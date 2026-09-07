/* Host tests for ad_dnssd.h's Known-Answer decision -- the SAME code the firmware runs.
 *
 * This function decides when NOT to answer an mDNS query, and every way it can be wrong
 * points the same direction: the badge goes quiet and stops being discoverable. This
 * project has had that failure twice and both times a human noticed before any
 * instrument did. So most of these tests are not "does it suppress when it should" --
 * they are "does it REFUSE to suppress in every case where it must not".
 *
 * Two kinds of fixture:
 *   - real queries captured off the air (testdata/dnssd_queries.inc). These are messages this code did not compose, which
 *     is the only way to test reading Apple's bytes.
 *   - synthetic messages built here, for the cases the air has not happened to produce:
 *     an expiring TTL, a pointer loop, a truncated record, a forward pointer.
 *
 *   cc -O2 -o /tmp/dnssd_test tools/dnssd_test.c && /tmp/dnssd_test
 */
#include <stdio.h>
#include <string.h>
#include "../ESP32Drop/src/airdrop/core/ad_dnssd.h"

static int fails = 0, ran = 0;
static void check(const char *name, int ok, const char *detail) {
  ran++;
  printf("  [%s] %-62s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}

static const struct AdDnssdOwn OWN = { "2a848543b30c", "M5 Badge" };

enum { AD_Q_ALL_KNOWN, AD_Q_SOME_KNOWN, AD_Q_NONE_KNOWN, AD_Q_NOT_OURS };
static const char *CLASSNAME[] = { "ALL_KNOWN", "SOME_KNOWN", "NONE_KNOWN", "NOT_OURS" };
struct Fixture { int cls; const char *src; const char *hex; };
static const struct Fixture FIX[] = {
#include "../testdata/dnssd_queries.inc"
};
#define NFIX (int)(sizeof(FIX) / sizeof(FIX[0]))

static int unhex(const char *h, uint8_t *out) {
  int n = 0;
  for (const char *q = h; q[0] && q[1]; q += 2) {
    int hi = (q[0] <= '9') ? q[0] - '0' : (q[0] | 32) - 'a' + 10;
    int lo = (q[1] <= '9') ? q[1] - '0' : (q[1] | 32) - 'a' + 10;
    out[n++] = (uint8_t)((hi << 4) | lo);
  }
  return n;
}

/* ---- a tiny message builder, for the cases the air did not hand us ---- */
static uint8_t B[512];
static int bn;
static void b_reset(int qd, int an) {
  memset(B, 0, sizeof B); bn = 12;
  B[6] = (uint8_t)(an >> 8); B[7] = (uint8_t)an;
  B[4] = (uint8_t)(qd >> 8); B[5] = (uint8_t)qd;
}
static void b_name(const char *dotted) {
  const char *s = dotted;
  while (*s) {
    const char *dot = strchr(s, '.');
    int l = dot ? (int)(dot - s) : (int)strlen(s);
    B[bn++] = (uint8_t)l; memcpy(B + bn, s, l); bn += l;
    if (!dot) break;
    s = dot + 1;
  }
  B[bn++] = 0;
}
static void b_question(const char *name, uint16_t type) {
  b_name(name);
  B[bn++] = (uint8_t)(type >> 8); B[bn++] = (uint8_t)type;
  B[bn++] = 0; B[bn++] = 1;
}
static void b_ptr(const char *name, const char *target, uint32_t ttl) {
  b_name(name);
  B[bn++] = 0; B[bn++] = AD_DNS_T_PTR;
  B[bn++] = 0; B[bn++] = 1;
  B[bn++] = (uint8_t)(ttl >> 24); B[bn++] = (uint8_t)(ttl >> 16);
  B[bn++] = (uint8_t)(ttl >> 8);  B[bn++] = (uint8_t)ttl;
  int rl = bn; bn += 2;
  int start = bn; b_name(target);
  B[rl] = (uint8_t)((bn - start) >> 8); B[rl + 1] = (uint8_t)(bn - start);
}

int main(void) {
  printf("ad_dnssd.h -- RFC 6762 7.1 known-answer suppression\n\n");
  static uint8_t m[2048];

  /* 1. Real captured queries, one assertion per class. Only ALL_KNOWN may suppress. */
  for (int i = 0; i < NFIX; i++) {
    int n = unhex(FIX[i].hex, m);
    bool sup = ad_dnssd_known_answer_suppresses(m, n, &OWN);
    bool want = (FIX[i].cls == AD_Q_ALL_KNOWN);
    char d[110];
    snprintf(d, sizeof d, "%s src=%s len=%d -> %s", CLASSNAME[FIX[i].cls], FIX[i].src, n,
             sup ? "suppress" : "reply");
    check(want ? "captured: every answer known -> SUPPRESS"
               : "captured: not fully known -> MUST REPLY", sup == want, d);
  }

  /* 2. The TTL condition. 7.1 only allows suppression while the known answer still has
     at least half our TTL; a nearly-expired one must not silence us, or we go missing
     precisely as the peer's cache is about to need refreshing. */
  {
    b_reset(1, 1);
    b_question("_airdrop._tcp.local", AD_DNS_T_PTR);
    b_ptr("_airdrop._tcp.local", "2a848543b30c._airdrop._tcp.local", AD_DNSSD_PTR_TTL);
    check("full TTL in the known answer -> suppress",
          ad_dnssd_known_answer_suppresses(B, bn, &OWN), NULL);

    b_reset(1, 1);
    b_question("_airdrop._tcp.local", AD_DNS_T_PTR);
    b_ptr("_airdrop._tcp.local", "2a848543b30c._airdrop._tcp.local", AD_DNSSD_PTR_TTL / 2);
    check("exactly half TTL -> still suppress (7.1 says at least half)",
          ad_dnssd_known_answer_suppresses(B, bn, &OWN), NULL);

    b_reset(1, 1);
    b_question("_airdrop._tcp.local", AD_DNS_T_PTR);
    b_ptr("_airdrop._tcp.local", "2a848543b30c._airdrop._tcp.local", AD_DNSSD_PTR_TTL / 2 - 1);
    check("just under half TTL -> MUST REPLY",
          !ad_dnssd_known_answer_suppresses(B, bn, &OWN), NULL);
  }

  /* 3. A known answer naming SOMEBODY ELSE'S instance must not silence us. This is the
     exact shape of the captured Mac query: it knows its own pairing instances, not ours. */
  {
    b_reset(1, 1);
    b_question("_airdrop._tcp.local", AD_DNS_T_PTR);
    b_ptr("_airdrop._tcp.local", "3219d6124af2._airdrop._tcp.local", AD_DNSSD_PTR_TTL);
    check("a known answer for another device -> MUST REPLY",
          !ad_dnssd_known_answer_suppresses(B, bn, &OWN), NULL);
  }

  /* 4. Partial knowledge. Three questions, only one answer known: we still owe two
     records, so we reply. Suppressing here would be the silent-disappearance bug. */
  {
    b_reset(3, 1);
    b_question("_airdrop._tcp.local", AD_DNS_T_PTR);
    b_question("_applicationServicePairing._tcp.local", AD_DNS_T_PTR);
    b_question("_appSvcPrePair._tcp.local", AD_DNS_T_PTR);
    b_ptr("_airdrop._tcp.local", "2a848543b30c._airdrop._tcp.local", AD_DNSSD_PTR_TTL);
    check("one of three questions known -> MUST REPLY",
          !ad_dnssd_known_answer_suppresses(B, bn, &OWN), NULL);

    b_reset(3, 3);
    b_question("_airdrop._tcp.local", AD_DNS_T_PTR);
    b_question("_applicationServicePairing._tcp.local", AD_DNS_T_PTR);
    b_question("_appSvcPrePair._tcp.local", AD_DNS_T_PTR);
    b_ptr("_airdrop._tcp.local", "2a848543b30c._airdrop._tcp.local", AD_DNSSD_PTR_TTL);
    b_ptr("_applicationServicePairing._tcp.local", "M5 Badge._applicationServicePairing._tcp.local", AD_DNSSD_PTR_TTL);
    b_ptr("_appSvcPrePair._tcp.local", "M5 Badge._appSvcPrePair._tcp.local", AD_DNSSD_PTR_TTL);
    check("all three known -> suppress",
          ad_dnssd_known_answer_suppresses(B, bn, &OWN), NULL);
  }

  /* 5. A question about our own instance is answered by SRV/TXT/AAAA, which a PTR
     known-answer list cannot contain. It must always win, even alongside a known PTR --
     this is the Mac resolving us just before it connects, the single most important
     query we receive. */
  {
    b_reset(2, 1);
    b_question("_airdrop._tcp.local", AD_DNS_T_PTR);
    b_question("2a848543b30c._airdrop._tcp.local", AD_DNS_T_SRV);
    b_ptr("_airdrop._tcp.local", "2a848543b30c._airdrop._tcp.local", AD_DNSSD_PTR_TTL);
    check("a resolve for our SRV always wins over a known PTR",
          !ad_dnssd_known_answer_suppresses(B, bn, &OWN), NULL);

    b_reset(2, 1);
    b_question("_airdrop._tcp.local", AD_DNS_T_PTR);
    b_question("2a848543b30c.local", AD_DNS_T_AAAA);
    b_ptr("_airdrop._tcp.local", "2a848543b30c._airdrop._tcp.local", AD_DNSSD_PTR_TTL);
    check("a resolve for our AAAA always wins over a known PTR",
          !ad_dnssd_known_answer_suppresses(B, bn, &OWN), NULL);
  }

  /* 6. Nothing of ours asked: NOT this build's business. Returning false leaves the
     existing (defective) behaviour untouched, which keeps this change to one thing. */
  {
    b_reset(1, 1);
    b_question("_companion-link._tcp.local", AD_DNS_T_PTR);
    b_ptr("_companion-link._tcp.local", "somebody._companion-link._tcp.local", AD_DNSSD_PTR_TTL);
    check("a query about nothing of ours -> not suppressed here (separate defect)",
          !ad_dnssd_known_answer_suppresses(B, bn, &OWN), NULL);
  }

  /* 7. Malformation must never suppress. Every one of these would, if mishandled,
     silence the badge on a peer's malformed packet. */
  {
    b_reset(1, 1);
    b_question("_airdrop._tcp.local", AD_DNS_T_PTR);
    b_ptr("_airdrop._tcp.local", "2a848543b30c._airdrop._tcp.local", AD_DNSSD_PTR_TTL);
    int full = bn;
    for (int cut = 13; cut < full; cut += 7)
      if (ad_dnssd_known_answer_suppresses(B, cut, &OWN)) {
        char d[64]; snprintf(d, sizeof d, "suppressed at truncation %d of %d", cut, full);
        check("no truncation of a suppressing message may suppress", 0, d);
        goto trunc_done;
      }
    check("no truncation of a suppressing message may suppress", 1, "all cut points reply");
  trunc_done:;

    /* a name that points at itself */
    b_reset(1, 1);
    int qn = bn;
    B[bn++] = 0xc0; B[bn++] = (uint8_t)qn;              /* pointer to itself */
    B[bn++] = 0; B[bn++] = AD_DNS_T_PTR; B[bn++] = 0; B[bn++] = 1;
    b_ptr("_airdrop._tcp.local", "2a848543b30c._airdrop._tcp.local", AD_DNSSD_PTR_TTL);
    check("a self-referential name pointer does not suppress (and returns)",
          !ad_dnssd_known_answer_suppresses(B, bn, &OWN), NULL);

    /* a forward pointer -- legal-looking, but only backward jumps can terminate */
    b_reset(1, 1);
    int q2 = bn;
    B[bn++] = 0xc0; B[bn++] = (uint8_t)(q2 + 32);
    B[bn++] = 0; B[bn++] = AD_DNS_T_PTR; B[bn++] = 0; B[bn++] = 1;
    b_ptr("_airdrop._tcp.local", "2a848543b30c._airdrop._tcp.local", AD_DNSSD_PTR_TTL);
    check("a forward name pointer does not suppress",
          !ad_dnssd_known_answer_suppresses(B, bn, &OWN), NULL);
  }

  /* 8. Shape guards. */
  {
    b_reset(1, 0);
    b_question("_airdrop._tcp.local", AD_DNS_T_PTR);
    check("a query with no known answers -> MUST REPLY",
          !ad_dnssd_known_answer_suppresses(B, bn, &OWN), NULL);

    b_reset(1, 1);
    b_question("_airdrop._tcp.local", AD_DNS_T_PTR);
    b_ptr("_airdrop._tcp.local", "2a848543b30c._airdrop._tcp.local", AD_DNSSD_PTR_TTL);
    B[2] = 0x84;                                        /* mark it a response */
    check("a RESPONSE is never suppressed (it is not a query)",
          !ad_dnssd_known_answer_suppresses(B, bn, &OWN), NULL);

    check("a message shorter than a header does not suppress",
          !ad_dnssd_known_answer_suppresses(B, 11, &OWN), NULL);
  }

  /* 9. DNS names are case-insensitive; a peer that upper-cases must still suppress us. */
  {
    b_reset(1, 1);
    b_question("_AirDrop._TCP.local", AD_DNS_T_PTR);
    b_ptr("_AIRDROP._tcp.LOCAL", "2A848543B30C._airdrop._tcp.local", AD_DNSSD_PTR_TTL);
    check("names compare case-insensitively",
          ad_dnssd_known_answer_suppresses(B, bn, &OWN), NULL);
  }

  /* 10. QTYPE=ANY on a service name behaves as a PTR browse. */
  {
    b_reset(1, 1);
    b_question("_airdrop._tcp.local", AD_DNS_T_ANY);
    b_ptr("_airdrop._tcp.local", "2a848543b30c._airdrop._tcp.local", AD_DNSSD_PTR_TTL);
    check("QTYPE=ANY on the service name suppresses like PTR",
          ad_dnssd_known_answer_suppresses(B, bn, &OWN), NULL);
  }

  /* ---------------- ad_dnssd_asks_us: the filter ---------------- */

  /* 11. Every captured query, classified. The corpus generator already labels which ask
     for nothing of ours; the parser must agree with it. */
  {
    int ours = 0, not_ours = 0, wrong = 0;
    for (int i = 0; i < NFIX; i++) {
      int n = unhex(FIX[i].hex, m);
      bool asks = ad_dnssd_asks_us(m, n, &OWN);
      bool want = (FIX[i].cls != AD_Q_NOT_OURS);
      if (asks != want) { wrong++;
        printf("      mismatch: %s src=%s asks_us=%d\n", CLASSNAME[FIX[i].cls], FIX[i].src, asks); }
      if (asks) ours++; else not_ours++;
    }
    char d[80]; snprintf(d, sizeof d, "%d ours, %d not ours, %d mismatched", ours, not_ours, wrong);
    check("captured queries classify as the corpus labels them", wrong == 0, d);
  }

  /* 12. The defect itself: another device resolving ITS OWN instance. The old substring
     filter matched this because the message contains "_airdrop", and we answered with
     our whole response. */
  {
    b_reset(1, 0);
    b_question("3219d6124af2._airdrop._tcp.local", AD_DNS_T_SRV);
    check("a foreign instance resolve is NOT ours",
          !ad_dnssd_asks_us(B, bn, &OWN), NULL);

    b_reset(1, 0);
    b_question("3219d6124af2.local", AD_DNS_T_AAAA);
    check("a foreign host resolve is NOT ours", !ad_dnssd_asks_us(B, bn, &OWN), NULL);
  }

  /* 13. Everything that IS ours, at the types actually seen on the air. */
  {
    const struct { const char *n; uint16_t t; } MINE[] = {
      { "_airdrop._tcp.local",                        AD_DNS_T_PTR  },
      { "_applicationServicePairing._tcp.local",      AD_DNS_T_PTR  },
      { "_appSvcPrePair._tcp.local",                  AD_DNS_T_PTR  },
      { "_services._dns-sd._udp.local",               AD_DNS_T_PTR  },
      { "_airdrop._tcp.local",                        AD_DNS_T_ANY  },
      { "2a848543b30c._airdrop._tcp.local",           AD_DNS_T_SRV  },
      { "2a848543b30c._airdrop._tcp.local",           AD_DNS_T_TXT  },
      { "2a848543b30c._airdrop._tcp.local",           AD_DNS_T_ANY  },
      { "2a848543b30c.local",                         AD_DNS_T_AAAA },
      { "2a848543b30c.local",                         1             },  /* A: we lack it,
                                                          but our NSEC answers it */
      { "M5 Badge._applicationServicePairing._tcp.local", AD_DNS_T_SRV },
      { "M5 Badge._appSvcPrePair._tcp.local",             AD_DNS_T_TXT },
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof MINE / sizeof MINE[0]; i++) {
      b_reset(1, 0); b_question(MINE[i].n, MINE[i].t);
      if (!ad_dnssd_asks_us(B, bn, &OWN)) {
        bad++; printf("      not recognised: %s type=%u\n", MINE[i].n, MINE[i].t);
      }
    }
    check("every name we hold is recognised as ours", bad == 0, NULL);
  }

  /* 14. A query mixing ours and a stranger's still gets answered. */
  {
    b_reset(2, 0);
    b_question("3219d6124af2._airdrop._tcp.local", AD_DNS_T_SRV);
    b_question("_airdrop._tcp.local", AD_DNS_T_PTR);
    check("one of ours among strangers -> still ours", ad_dnssd_asks_us(B, bn, &OWN), NULL);
  }

  /* 15. Doubt answers. Every one of these would, if it returned false, silence us on a
     peer's malformed packet -- so all of them must say "ours" and let the old behaviour
     stand. */
  {
    b_reset(1, 0);
    b_question("3219d6124af2._airdrop._tcp.local", AD_DNS_T_SRV);
    int full = bn, bad = 0;
    for (int cut = 12; cut < full; cut++)
      if (!ad_dnssd_asks_us(B, cut, &OWN)) bad++;
    char d[64]; snprintf(d, sizeof d, "%d of %d truncations said 'not ours'", bad, full - 12);
    check("no truncation of a foreign query may say 'not ours'", bad == 0, d);

    b_reset(1, 0);
    int qn2 = bn; B[bn++] = 0xc0; B[bn++] = (uint8_t)qn2;    /* self-pointer */
    B[bn++] = 0; B[bn++] = AD_DNS_T_PTR; B[bn++] = 0; B[bn++] = 1;
    check("a self-referential name says 'ours' (answer as before)",
          ad_dnssd_asks_us(B, bn, &OWN), NULL);

    b_reset(0, 0);
    check("a query with no questions says 'ours'", ad_dnssd_asks_us(B, bn, &OWN), NULL);
    check("a message shorter than a header says 'ours'", ad_dnssd_asks_us(B, 8, &OWN), NULL);

    b_reset(1, 0); b_question("_airdrop._tcp.local", AD_DNS_T_PTR); B[2] = 0x84;
    check("a response says 'ours' (not this function's business)",
          ad_dnssd_asks_us(B, bn, &OWN), NULL);
  }

  printf("\n%d checks, %d failed\n", ran, fails);
  return fails ? 1 : 0;
}
