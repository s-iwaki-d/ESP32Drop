#include <assert.h>
#include <stdio.h>
#include "../ESP32Drop/src/awdl/core/awdl_transfer_trace.h"
#include "../ESP32Drop/src/airdrop/core/ad_dnssd.h"

static int unhex(const char *s, uint8_t *p) {
  int n = 0; unsigned x;
  while (*s) { assert(sscanf(s, "%2x", &x) == 1); p[n++] = (uint8_t)x; s += 2; }
  return n;
}
int main(void) {
  uint8_t ip[256] = {0}; struct AwdlTransferEvent e;
  // The real three-record goodbye captured from an iPhone.
  int dn = unhex("0000840000000003000000000e5f61707053766350726550616972045f746370056c6f63616c00000c000100000000000d0a4d6950686f6e65416972c00c1a5f6170706c69636174696f6e5365727669636550616972696e67c01b000c000100000000000d0a4d6950686f6e65416972c03e085f61697264726f70c01b000c000100000000000f0c343135393463663563356366c072", ip + 48);
  // Only record-name/TTL parsing is relevant; RDATA is opaque to this instrument.
  ip[0] = 0x60; ip[6] = 17; ip[5] = (uint8_t)(dn + 8);
  ip[40] = ip[42] = 0x14; ip[41] = ip[43] = 0xe9; ip[45] = ip[5];
  assert(awdl_trace_classify(ip, dn + 48, &e) && e.kind == 17 && e.zero_ttl == 3);
  struct AdDnssdOwn own = {"2a8485450bc0", "M5 Badge"};
  assert(ip[50] & 0x80); // Existing RX tap's is_query branch is NOT entered.
  assert(!ad_dnssd_known_answer_suppresses(ip + 48, dn, &own));
  for (int n = 0; n < dn + 48; ++n) assert(!awdl_trace_classify(ip, n, &e));
  ip[50] &= 0x7f; assert(!awdl_trace_classify(ip, dn + 48, &e)); ip[50] |= 0x80;
  ip[54] = 0xff; ip[55] = 0xff; assert(!awdl_trace_classify(ip, dn + 48, &e));
  ip[54] = 0; ip[55] = 3;
  // A malformed compression target is never chased and cannot hang the trace.
  ip[60] = 0xc0; ip[61] = 12; (void)awdl_trace_classify(ip, dn + 48, &e);

  memset(ip, 0, sizeof ip);
  int tn = unhex("6000000000140640fe8000000000000088d2fcfffea24935fe80000000000000288485fffe450bc02242c67b5a13aaf700000000500400003ee20000", ip);
  assert(tn == 60 && awdl_trace_classify(ip, tn, &e));
  assert(e.kind == 6 && e.flags == 4 && e.sport == 8770 && e.dport == 50811);
  assert(e.seq == 1511238391 && e.ack == 0 && e.size == 0);
  for (int n = 0; n < tn; ++n) assert(!awdl_trace_classify(ip, n, &e));
  ip[53] = 0x10; assert(!awdl_trace_classify(ip, tn, &e)); // ordinary ACK ignored
  ip[53] = 0x11; assert(awdl_trace_classify(ip, tn, &e)); // FIN
  ip[53] = 2; assert(awdl_trace_classify(ip, tn, &e)); // SYN
  ip[52] = 0xf0; assert(!awdl_trace_classify(ip, tn, &e));
  ip[52] = 0x40; assert(!awdl_trace_classify(ip, tn, &e));
  puts("transfer trace: captured goodbye/RST, truncations and malformed bounds PASS");
}
