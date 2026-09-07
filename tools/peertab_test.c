/* Host test for awdl_peertab.h. Compiled from the SAME header the firmware uses. */
#include <stdio.h>
#include <assert.h>
#include "awdl_peertab.h"
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c, msg) do { checks++; if (!(c)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)

int main(void) {
  printf("sizeof(struct AdPeerRow) = %zu\n", sizeof(struct AdPeerRow));
  printf("sizeof(struct AdPeerTab) = %zu  (%d rows)\n", sizeof(struct AdPeerTab), ADP_ROWS);

  struct AdPeerTab t; adp_reset(&t);
  const uint8_t A[6] = {2,0,0,0,0,1}, B[6] = {2,0,0,0,0,2};

  /* seeding: the first sample must BE the average, not be pulled up from zero */
  adp_frame(&t, A, -70, 1000);
  int i = adp_find(&t, A);
  CK(i >= 0, "peer A found after first frame");
  CK(t.row[i].rssi_last == -70, "rssi_last seeded");
  CK(t.row[i].rssi_avg  == -70, "rssi_avg seeded to the first sample, not 0");
  CK(t.row[i].first_ms == 1000 && t.row[i].last_ms == 1000, "timestamps set");
  CK(!t.row[i].saw_airdrop, "saw_airdrop starts false = unknown");

  /* approach: the average must track downward-in-magnitude (closer) monotonically */
  int8_t prev = t.row[i].rssi_avg;
  for (int k = 0; k < 12; k++) { adp_frame(&t, A, -40, 1000 + k); }
  CK(t.row[i].rssi_avg > prev, "rssi_avg rises toward the new, closer level");
  CK(t.row[i].rssi_avg <= -40, "rssi_avg never overshoots past the samples");
  CK(t.row[i].rssi_last == -40, "rssi_last is the latest, unsmoothed");

  /* a second peer is a second row, not an overwrite */
  adp_frame(&t, B, -90, 2000);
  CK(adp_find(&t, B) != adp_find(&t, A), "two MACs occupy two rows");

  /* service record marks reachability and takes the name once */
  adp_service(&t, A, 8770, 2100);
  i = adp_find(&t, A);
  CK(t.row[i].saw_airdrop, "saw_airdrop set by a service record");
  CK(t.row[i].port == 8770, "port recorded");

  /* a service record for an unseen MAC creates the row (order must not matter) */
  const uint8_t C[6] = {2,0,0,0,0,3};
  adp_service(&t, C, 8770, 2300);
  CK(adp_find(&t, C) >= 0, "service record alone creates a peer");

  /* eviction: filling past ADP_ROWS must drop the OLDEST and count it */
  adp_reset(&t);
  for (int k = 0; k < ADP_ROWS; k++) {
    uint8_t m[6] = {2,0,0,0,0,(uint8_t)(0x10 + k)};
    adp_frame(&t, m, -60, 1000 + k * 10);        /* k=0 is the oldest */
  }
  CK(t.dropped == 0, "no drops while there is room");
  uint8_t oldest[6] = {2,0,0,0,0,0x10}, fresh[6] = {2,0,0,0,0,0x99};
  adp_frame(&t, fresh, -50, 2000);
  CK(t.dropped == 1, "overflow counted");
  CK(adp_find(&t, oldest) < 0, "the least recently heard row was the one evicted");
  CK(adp_find(&t, fresh) >= 0, "the new peer got in");

  /* expiry: a peer quiet longer than the TTL must go, because the MAC has rotated */
  adp_reset(&t);
  adp_frame(&t, A, -60, 1000);
  adp_expire(&t, 1000 + 5000, 10000);
  CK(adp_find(&t, A) >= 0, "still present inside the TTL");
  adp_expire(&t, 1000 + 15000, 10000);
  CK(adp_find(&t, A) < 0, "expired past the TTL");

  /* AN UNHEARD PEER MUST NOT READ AS A NEAR ONE.
   *
   * A row created by adp_service() -- a peer known only because it announced
   * _airdrop._tcp, with no frame measured yet -- used to inherit an all-zero row, and zero
   * in rssi_avg is 0 dBm. That is stronger than the -14 dBm measured with a phone touching
   * the badge, so the peer nobody had heard from won every proximity comparison.
   * GreetingCard picks the strongest fresh row, so it would have dialled that one. */
  adp_reset(&t);
  { uint8_t svc[6] = {3,0,0,0,0,1};
    adp_service(&t, svc, 8770, 1000);
    int i = adp_find(&t, svc);
    CK(i >= 0, "a service record alone creates a row");
    CK(t.row[i].saw_airdrop, "...marked as having offered _airdrop._tcp");
    CK(t.row[i].rssi_avg == -128, "...with RSSI unknown, not 0 dBm");
    CK(t.row[i].rssi_avg < -25, "...so it cannot pass a near-enough test before being heard");
    /* and once a frame does arrive, the reading is the frame's, not an average with -128 */
    adp_frame(&t, svc, -40, 1100);
    CK(t.row[i].rssi_last == -40, "a first frame sets the last reading");
    CK(t.row[i].rssi_avg > -60, "...and the average follows it rather than crawling from -128"); }

  /* The same hazard through the other door: an EVICTED slot is memset to zero and handed
     out again, so a reused row must be unknown too, never 0 dBm. */
  adp_reset(&t);
  for (int k = 0; k < ADP_ROWS; k++) { uint8_t m[6] = {4,0,0,0,0,(uint8_t)k};
                                       adp_frame(&t, m, -60, 1000 + k * 10); }
  { uint8_t late[6] = {4,0,0,0,0,0xAA};
    adp_service(&t, late, 8770, 5000);      /* forces an eviction, then a service-only row */
    int i = adp_find(&t, late);
    CK(i >= 0, "the evicting peer got a row");
    CK(t.row[i].rssi_avg == -128, "a REUSED slot is unknown too, not the -60 it held before"); }

  printf("%d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
