/* Host tests for awdl_status.h -- the public pull API.
 *
 * What is worth testing here is not the struct. It is the two DERIVED answers, because
 * those are the ones a user will act on without checking the arithmetic behind them, and
 * because this project has twice shipped firmware that reported perfect health while
 * transmitting nothing. "Discoverable" therefore has to be defined as frames observed to
 * leave, never as a configuration that looks right.
 *
 *   cc -O2 -o /tmp/status_test tools/status_test.c -lm && /tmp/status_test
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../ESP32Drop/src/awdl/core/awdl_status.h"

static int fails = 0, ran = 0;
static void check(const char *name, int ok, const char *detail) {
  ran++;
  printf("  [%s] %-58s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}

static struct AwdlStatus healthy(void) {
  struct AwdlStatus s;
  memset(&s, 0, sizeof s);
  s.have_master = true; s.locked = true;
  s.ch6_mask = 0x0100; s.ch6_slot = 8; s.win_state = AWDL_WS_LOCKED;
  s.mif_sent = 400; s.mif_late = 0; s.win_served = 100; s.win_total = 100;
  s.ms_since_degrade = UINT32_MAX;   /* nothing has ever gone wrong */
  s.ms_since_mif = 120;          /* mid-window: a healthy device is never long silent */
  s.wander_us = 24; s.noise_rms_us = 18; s.drift_ppm = 13; s.samples = 500;
  s.peers = 2;
  return s;
}

int main(void) {
  printf("== awdl_status.h host tests ==\n");

  /* 1. The ceiling, which is the number that lets a user judge mif_sent without becoming
     an AWDL expert. Counted per RUN, because a mask is not one block: 0x3b00 is two ch6
     runs with their own lead and guard, and treating it as one gives 8 where the answer is
     21 -- an error that would tell a user their link was failing when it was perfect. */
  {
    struct { uint16_t mask; float want; const char *why; } C[] = {
      { 0x0100, 3.8147f, "one slot: 4 instants per cycle" },
      { 0x0300, 7.6294f, "two adjacent slots: 8" },
      { 0x3b00, 20.0272f, "slots 8-9 and 11-13: 8 + 13, NOT 8" },
      { 0x0000, 0.0f,     "no mask, no authority, no ceiling" },
    };
    for (unsigned i = 0; i < 4; i++) {
      float got = awdl_status_mif_ceiling(C[i].mask, 15000);
      char d[88]; snprintf(d, sizeof d, "0x%04x -> %.4f (%s)", C[i].mask, got, C[i].why);
      check("the ceiling counts instants per run, not per mask", fabsf(got - C[i].want) < 0.01f, d);
    }
    check("a zero MIF period has no ceiling rather than dividing by zero",
          awdl_status_mif_ceiling(0x0100, 0) == 0.0f, NULL);
  }

  /* 2. Discoverable. Each ingredient removed in turn, because the failure this guards
     against is a status that reads fine with one of them missing. */
  {
    struct AwdlStatus s = healthy();
    check("a healthy link is discoverable", awdl_status_discoverable(&s), NULL);
    s = healthy(); s.have_master = false;
    check("...not without a master", !awdl_status_discoverable(&s), NULL);
    s = healthy(); s.locked = false;
    check("...not without a lock", !awdl_status_discoverable(&s), NULL);
    s = healthy(); s.ch6_mask = 0;
    check("...not without a window we have authority for", !awdl_status_discoverable(&s), NULL);
    s = healthy(); s.mif_sent = 0;
    check("...and NOT when everything looks right but nothing has been sent",
          !awdl_status_discoverable(&s), "the failure this project shipped twice");
  }

  /* 3. Health, ordered so that > is worse, and with the two counts treated as exact. A
     percentage of missed instants would hide the first one, and the first one is the whole
     signal. */
  {
    struct AwdlStatus s = healthy();
    check("healthy reads ok", awdl_status_health(&s) == AWDL_HEALTH_OK, NULL);
    s = healthy(); s.mif_late = 1; s.ms_since_degrade = 0;
    check("ONE late instant is already degraded", awdl_status_health(&s) == AWDL_HEALTH_DEGRADED,
          "not a percentage: the first one is the signal");
    s = healthy(); s.win_served = 99; s.ms_since_degrade = 500;
    check("one unserved window is degraded", awdl_status_health(&s) == AWDL_HEALTH_DEGRADED, NULL);

    /* THE PROPERTY THIS VERDICT DID NOT HAVE. mif_late and win_served/win_total are
       lifetime totals, so a health reading built on them could be entered and never left:
       one missed window in the first minute left the device "degraded" for the rest of its
       uptime. Measured on hardware: run=15919/17394 after five hours, still
       reported degraded. A reading that cannot say "not any more" is a memory, not a
       health reading. */
    s = healthy(); s.mif_late = 1475; s.win_served = 15919; s.win_total = 17394;
    s.ms_since_degrade = AWDL_DEGRADE_MAX_MS + 1;
    { char dt[96]; snprintf(dt, sizeof dt, "late=%u served=%u/%u since=%ums",
                            s.mif_late, s.win_served, s.win_total, s.ms_since_degrade);
      check("a link that HAS been unwell but is well NOW reads ok -- the verdict recovers",
            awdl_status_health(&s) == AWDL_HEALTH_OK, dt); }
    s.ms_since_degrade = AWDL_DEGRADE_MAX_MS;
    check("...and one millisecond inside the window it is still degraded",
          awdl_status_health(&s) == AWDL_HEALTH_DEGRADED, NULL);
    s = healthy(); s.mif_late = 999999; s.win_served = 0; s.win_total = 999999;
    check("...and lifetime totals alone no longer decide anything",
          awdl_status_health(&s) == AWDL_HEALTH_OK, NULL);
    s = healthy(); s.ms_since_degrade = 0; s.ms_since_mif = AWDL_SILENCE_MAX_MS + 1;
    check("silence still outranks degraded -- nothing on the air is the worse fact",
          awdl_status_health(&s) == AWDL_HEALTH_SILENT, NULL);
    s = healthy(); s.locked = false;
    check("a master we cannot track is no-lock", awdl_status_health(&s) == AWDL_HEALTH_NO_LOCK, NULL);
    s = healthy(); s.have_master = false;
    check("no master outranks everything else", awdl_status_health(&s) == AWDL_HEALTH_NO_MASTER, NULL);
    s = healthy(); s.ch6_mask = 0; s.locked = false; s.mif_late = 5;
    check("no window is reported as no-master, not as degraded",
          awdl_status_health(&s) == AWDL_HEALTH_NO_MASTER,
          "an empty mask means no authority, which is the same problem");
    /* the ordering is the contract: a caller may compare */
    check("the health values are ordered worst-last",
          AWDL_HEALTH_OK < AWDL_HEALTH_DEGRADED &&
          AWDL_HEALTH_DEGRADED < AWDL_HEALTH_NO_LOCK &&
          AWDL_HEALTH_NO_LOCK < AWDL_HEALTH_NO_MASTER, NULL);
    for (int h = 0; h <= AWDL_HEALTH_NO_MASTER; h++)
      if (!awdl_status_health_name(h)[0]) check("every health value has a name", 0, NULL);
    check("every health value has a name", 1, "ok / degraded / silent / no-lock / no-master");
  }

  /* RECENCY. This block is the reason the file changed: the predicate above it used to
     test mif_sent > 0, which is CUMULATIVE, so a single frame at boot made the device
     "discoverable" for ever. The failure it was written to catch -- a dead cadence task --
     is exactly the one it could not see, because every other counter freezes too. */
  {
    struct AwdlStatus s = healthy();

    s.ms_since_mif = 0;
    check("discoverable: a frame this instant", awdl_status_discoverable(&s), NULL);

    /* The lower bound is not arbitrary. A width-1 mask opens its window once per
       1,048,576 us cycle, so a PERFECT device is silent for ~996 ms between windows. A
       threshold at or below one second would alarm on correct operation. */
    s.ms_since_mif = 996;
    check("996 ms of silence is the width-1 gap, not a fault", awdl_status_discoverable(&s),
          "the natural gap between availability windows");

    s.ms_since_mif = AWDL_SILENCE_MAX_MS;
    check("silence exactly at the threshold is still discoverable", awdl_status_discoverable(&s), NULL);

    s.ms_since_mif = AWDL_SILENCE_MAX_MS + 1;
    check("one ms past the threshold is NOT discoverable", !awdl_status_discoverable(&s), NULL);

    /* THE BUG, as a test. Everything a dead cadence task leaves behind, verbatim: a master,
       a lock, a mask, frames sent at some point, mif_late frozen at 0 and win_served frozen
       equal to win_total. Only the clock has moved. */
    s = healthy();
    s.ms_since_mif = 30000;                 /* half a minute; macOS drops us after 6-8 s */
    check("a dead transmit path is NOT discoverable, though every counter reads perfect",
          !awdl_status_discoverable(&s), "mif_sent=400 mif_late=0 win 100/100");
    check("...and health says SILENT, not OK",
          awdl_status_health(&s) == AWDL_HEALTH_SILENT, awdl_status_health_name(awdl_status_health(&s)));

    /* Silence must be checked BEFORE the frozen counters, or it can never be reached. */
    s.mif_late = 7;
    check("silence outranks degraded, because degraded is what freezing looks like",
          awdl_status_health(&s) == AWDL_HEALTH_SILENT, NULL);

    /* Never having transmitted at all is its own thing and must not read as discoverable. */
    s = healthy();
    s.mif_sent = 0; s.ms_since_mif = UINT32_MAX;
    check("a device that has never transmitted is not discoverable", !awdl_status_discoverable(&s), NULL);
    check("...and is SILENT rather than OK", awdl_status_health(&s) == AWDL_HEALTH_SILENT, NULL);

    /* Losing the master still outranks silence: it is not our fault and it is not the
       same advice to the user. */
    s = healthy(); s.ms_since_mif = 30000; s.have_master = false;
    check("no master outranks silence", awdl_status_health(&s) == AWDL_HEALTH_NO_MASTER, NULL);
    s = healthy(); s.ms_since_mif = 30000; s.locked = false;
    check("no lock outranks silence", awdl_status_health(&s) == AWDL_HEALTH_NO_LOCK, NULL);
  }

  /* The episode clock, which is the shape both no_master_ms and ms_since_mif are built on.
     no_master_ms used to be assigned the accumulated dwell of election FALLBACK episodes:
     a lifetime total, measured while a master IS present, that never returned to zero.
     "How long has this been true" and "how much of this has there ever been" are different
     questions with the same units, which is how they got confused. */
  {
    check("not in an episode reads zero, whatever the timestamps say",
          awdl_episode_ms(false, 12345, 99999) == 0, NULL);
    check("an episode that just started reads zero",
          awdl_episode_ms(true, 5000, 5000) == 0, NULL);
    check("an ordinary episode", awdl_episode_ms(true, 5000, 12500) == 7500, NULL);

    /* The 49.7-day rollover. uint32 milliseconds were chosen because they cross a task
       priority boundary and a 64-bit read on a 32-bit core can tear; the rollover is the
       price, and unsigned subtraction pays it exactly. */
    check("across the 32-bit wrap", awdl_episode_ms(true, 0xFFFFF000u, 0x00000100u) == 0x1100u,
          "started 4096 ms before the wrap, now 256 ms after it");
    check("exactly at the wrap", awdl_episode_ms(true, 0xFFFFFFFFu, 0x00000000u) == 1, NULL);
    check("a full period reads zero, not a negative", awdl_episode_ms(true, 7, 7) == 0, NULL);
  }

  printf("\n%d checks, %d failed\n", ran, fails);
  return fails ? 1 : 0;
}
