/* GreetingCard -- hand someone a picture by bringing your badge near them.
 *
 * The badge watches for Apple devices nearby. When one comes close enough that it reads as
 * a deliberate gesture -- near enough to touch -- it offers that device a small image over
 * AirDrop. The other person sees their normal AirDrop prompt and decides.
 *
 * Transfers of this card (well under 128 KiB) to a Mac and to an iPhone are both measured
 * working. Two things about iOS are worth knowing anyway, because they shape what the badge can do: its AirDrop listener
 * closes about 25 seconds after the wake advertisement stops (a Mac's stays open past 40),
 * which is why AD_CYCLE_WATCH_MS is 20 s; and its "Everyone" setting reverts to "Contacts
 * Only" after ten minutes, and this badge has no Apple ID, so it is never a contact --
 * an iPhone left alone will stop accepting until somebody sets Everyone again.
 *
 * WHAT THIS SKETCH IS HONEST ABOUT
 *
 * Nothing here bypasses anyone's settings. AirDrop's "Everyone" mode is opt-in on the
 * RECEIVING device, the receiver's own Accept dialog is what actually moves the file, and a
 * person who ignores it gets nothing. Proximity is the trigger precisely so that the offer
 * only happens when somebody has chosen to come close.
 *
 * WHY THE BADGE SPENDS HALF ITS TIME BLIND
 *
 * An Apple device's AirDrop listener is not running until a Continuity BLE advertisement
 * wakes it, and it closes again some time after that advertisement stops. The BLE
 * controller and the AWDL radio cannot both be resident: they
 * cost about 26 KB and 110 KB of internal memory and a device holding both has nothing left
 * to be a badge with. So the sketch alternates: advertise, release BLE, bring AWDL up, look
 * for someone close, send, tear AWDL down, repeat. ad_cycle_poll() runs that loop, and
 * during its BLE phase no peer can be seen at all. The screen says so.
 *
 * ESP32DROP_SENDER
 *
 * This one define decides what the library is. With it, the BLE advertiser and the send
 * path compile in; without it, they are not merely unused but absent -- and the receiving
 * half is absent here in the same way. The two roles cannot share a device: a listener's
 * 32 KB stack and a TLS client context do not both fit beside AWDL with anything left to
 * be a badge with, so the build makes the choice rather than leaving it to be discovered at
 * runtime.
 *
 * ⚠️ IT HAS TO BE IN build_opt.h, NOT HERE. A #define in a .ino reaches the sketch and
 * nothing else: Arduino compiles each library .cpp as its own translation unit, without the
 * sketch's macros. This example carried the define in the sketch alone, and the result
 * compiled and linked and could not work -- ad_cycle.cpp took its #else branch, so
 * ad_cycle_begin() was the stub that returns -1, BLEAdvMin.cpp contained no advertiser, and
 * nothing ever woke a peer. Verified by symbol: the compiled ad_cycle object held none of
 * the file statics the real implementation defines.
 *
 * build_opt.h beside this sketch is the mechanism that does reach the library. Its contents
 * are passed to every compilation, so the library sees the define too. Keep the #define
 * below as well: it is what makes the sketch's own uses of ad_cycle.h compile.
 */
#define ESP32DROP_SENDER
#include <M5Unified.h>
#include <ESP32Drop.h>
#include "card_png.h"   /* CARD_PNG, CARD_PNG_LEN -- see tools/gen-card.py */

/* HOW CLOSE IS "CLOSE ENOUGH". Measured, not guessed.
 *
 * An iPhone was walked up to the badge while rssi_avg was logged, with a laptop sitting
 * still across the room as a control:
 *
 *     across the room   -49 to -41 dBm, 41 samples, clustered hard at -43
 *     on the way in     -39 -38 -35 -34 -33 -31 -23 -21, one or two samples each
 *     touching          -14 dBm, 29 samples -- a flat plateau, not a spike
 *
 * Twenty-nine decibels separate "in the room" from "in your hand", which is what makes
 * this usable as a deliberate gesture rather than a proximity guess. -25 sits well below
 * the plateau, above every transient on the way in, and more than 6 dB clear of the
 * standing room level, so a device across the room cannot trip it however it is turned.
 *
 * ⚠️ RE-MEASURE ON YOUR OWN HARDWARE AND FOR THE DEVICE YOU CARE ABOUT. The screen prints
 * rssi_avg for exactly this purpose. A phone and a laptop do not read the same at the same
 * distance, and antenna orientation, hands and bodies all move the number. */
#ifndef NEAR_DBM              /* overridable: -DNEAR_DBM=... to try a value without editing */
#define NEAR_DBM  -30
#endif
#ifndef FRESH_MS
#define FRESH_MS  2000
#endif         /* ignore a peer not heard from this recently */

/* Do not offer the same person the same card again for ten minutes. The key is the name
 * from /Discover when there is one -- an AWDL MAC rotates about every 100 seconds, so it
 * cannot serve as an identity. When there is no name, the MAC is all there is, and a
 * duplicate offer is a smaller harm than never sending at all. */
#ifndef REPEAT_MS
#define REPEAT_MS 600000UL
#endif
static uint8_t  g_last_mac[6];
static uint32_t g_last_ms;

/* NOT EVERY NEARBY DEVICE IS A RECEIVER, and the strongest signal is not the likeliest one.
 *
 * Measured: a device in the room answers a connect to :8770 with RST (errno 104) every time
 * -- it runs AWDL for something that is not AirDrop. It is also often the closest thing to
 * the badge. Picking purely by signal strength therefore locks onto it and never reaches the
 * device that would have accepted. So a peer that just refused is set aside for a while and
 * the next candidate gets a turn. */
#define SNUB_MS 30000UL
static uint8_t  g_snub_mac[6];
static uint32_t g_snub_ms;

/* AD_SEND_FAILED and AD_SEND_DONE latch until the next ad_send(), so a slow loop cannot miss
 * the ending. That means the sketch, not the library, decides when to move on -- hold the
 * result on screen long enough to be read, then let the next candidate be considered. */
#define RESULT_HOLD_MS 3000UL
/* Why the last pick() declined somebody who WAS near. The screen showed "close!" and then
   did nothing, and the two are computed from different tests -- close! counts rows over
   NEAR_DBM, pick() also wants them fresh, not just greeted, and not snubbed. Without this
   the disagreement is invisible and reads as the send silently failing. */
static const char *g_why = nullptr;

static uint32_t g_result_ms;
static int      g_last_state = -1;
static int      g_last_reported = -1;


/* ---- the screen -------------------------------------------------------------------- */

/* The badge is blind for roughly half of every cycle, and a person holding it has no way to
 * know that unless it says so. A screen that goes dark during the BLE phase is the same
 * sketch failing silently: someone walks up, nothing happens, and the badge looks broken
 * when it is working exactly as designed. So the phase is always on the display, with the
 * time left in it. */
static void show(const char *msg, int code) {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(4, 8);
  M5.Display.setTextColor(TFT_RED);
  M5.Display.printf("%s", msg);
  if (code) { M5.Display.setCursor(4, 28); M5.Display.printf("%d", code); }
}

static const char *send_line(const struct AdSendStatus *s) {
  switch (s->state) {
    case AD_SEND_DISCOVER: return "asking if they can...";
    case AD_SEND_HOLD_:    return "deciding";
    case AD_SEND_ASK:      return "waiting for them";   /* their dialog is up */
    case AD_SEND_UPLOAD:   return "sending";
    case AD_SEND_DONE:     return "sent!";
    case AD_SEND_FAILED:   return "not sent";
    default:               return nullptr;
  }
}

static const char *fail_line(int err) {
  switch (err) {
    case ADS_ERR_CONNECT_TIMEOUT:  return "too late - they slept";
    case ADS_ERR_CONNECT_REFUSED:  return "refused";
    case ADS_ERR_TLS:              return "tls";
    case ADS_ERR_RESPONSE_TIMEOUT: return "no answer";
    case ADS_ERR_PEER_CLOSED:      return "they hung up";
    case ADS_ERR_STALLED:          return "stalled";
    case ADS_ERR_HTTP_STATUS:      return "declined or refused";
    case ADS_ERR_NOT_AIRDROPABLE:  return "not accepting";
    case ADS_ERR_NO_MEM:           return "out of memory";
    case ADS_ERR_ABORTED:          return "skipped";
    default:                       return "";
  }
}

/* CENTRED BOTH WAYS, because the StopWatch's screen is a CIRCLE. Text laid out from a
 * top-left cursor loses its first line to the bezel -- the corners of a round display are
 * simply not there. So the lines are collected first and then drawn around the middle,
 * which costs a small buffer and makes every line visible whatever the count. */
#define CARD_LINES 6
#define CARD_LINE_LEN 24

static void draw(const struct AdSendStatus *s) {
  static uint32_t last = 0;
  if (millis() - last < 250) return;         /* 4 Hz: ad_send_poll() sets the transfer rate,
                                                so a slow draw costs speed, not correctness */
  last = millis();

  char L[CARD_LINES][CARD_LINE_LEN];
  int n = 0;
  #define PUT(...) do { if (n < CARD_LINES) snprintf(L[n++], CARD_LINE_LEN, __VA_ARGS__); } while (0)

  /* A RESULT IS NEWS, NOT A STATE. DONE and FAILED latch in the library until the next
     ad_send() -- deliberately, so a slow polling loop cannot miss an ending -- and this
     screen used to render that latch directly. So after one failure it showed "not sent"
     for ever: the logic went idle again after RESULT_HOLD_MS and started looking, while the
     display stayed frozen on the old answer and never returned to the readout that says
     what is nearby and how strong it is. Anyone trying to work out why nothing was being
     sent was reading a screen that had stopped reporting. It expires here on the same
     clock the retry uses. */
  const bool result_stale = (s->state == AD_SEND_DONE || s->state == AD_SEND_FAILED) &&
                            (millis() - g_result_ms >= RESULT_HOLD_MS);
  const char *sl = result_stale ? nullptr : send_line(s);
  if (sl) {
    PUT("%s", sl);
    if (s->state == AD_SEND_UPLOAD && s->total)
      PUT("%lu%%", (unsigned long)(100UL * s->sent / s->total));
    if (s->state == AD_SEND_FAILED) {
      PUT("%s", fail_line(s->err));
      /* WHICH connect failed changes what it means: at DISCOVER the peer was never
         listening, at UPLOAD it stopped listening while a human decided. And sock_errno
         separates "nobody answered" (0) from "this device could not make a socket". */
      PUT("in %s%s",
          s->failed_in == AD_SEND_DISCOVER ? "discover" :
          s->failed_in == AD_SEND_ASK      ? "ask"      :
          s->failed_in == AD_SEND_UPLOAD   ? "upload"   : "?",
          s->sock_errno ? " (local)" : "");
    }
    if (s->peer_name[0]) PUT("%.23s", s->peer_name);   /* one screen line; the name is 64 bytes */
  } else {
    switch (ad_cycle_phase()) {
      case AD_CYCLE_WAKING:
        PUT("waking nearby");
        PUT("devices...");
        PUT("%lus", (unsigned long)(ad_cycle_phase_left_ms() / 1000));
        break;
      case AD_CYCLE_LOCKING:
        PUT("joining...");
        break;
      case AD_CYCLE_WATCHING: {
        struct AdPeerTab t; ad_peers_read(&t);
        int cnt = 0, near = 0;
        for (int i = 0; i < ADP_ROWS; i++) {
          if (!t.row[i].used) continue;
          cnt++;
          if (t.row[i].rssi_avg >= NEAR_DBM) near++;
        }
        PUT("bring me close");
        PUT("%d nearby", cnt);
        /* The tuning aid: the number you need in order to choose NEAR_DBM. -128 means the
           peer has only been heard ABOUT, never heard from -- see awdl_peertab.h. */
        /* "ad" marks a peer heard offering _airdrop._tcp. pick() does NOT require it --
           false means "has not said so yet", never "cannot" -- but when nothing is being
           sent it is the first thing worth knowing about whoever is close. */
        for (int i = 0; i < ADP_ROWS && n < CARD_LINES - 1; i++)
          if (t.row[i].used) PUT("%d dBm%s", (int)t.row[i].rssi_avg,
                                 t.row[i].saw_airdrop ? " ad" : "");
        if (near) PUT("close!%s%s", g_why ? " " : "", g_why ? g_why : "");
        break;
      }
      default:
        PUT("...");
        break;
    }
  }
  #undef PUT

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextDatum(middle_center);
  const int lh = M5.Display.fontHeight() + 2;
  const int cx = M5.Display.width() / 2;
  int y = M5.Display.height() / 2 - ((n - 1) * lh) / 2;
  for (int i = 0; i < n; i++, y += lh) M5.Display.drawString(L[i], cx, y);
}


void setup() {
  /* The config form, and setRotation, are both required on this hardware: a bare
     M5.begin() leaves the panel dark. */
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0);
  M5.Display.setTextSize(2);
  M5.Display.fillScreen(TFT_BLACK);

  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[GreetingCard] starting\n");

  /* FIRST, before any radio. The TLS client context is one contiguous 33 KB block, and
     after the radios have been up and down once there is no longer 33 KB in one piece --
     the same allocation has failed with 39,560 bytes still free. */
  int r = ad_sender_begin("M5 Badge");
  Serial.printf("[GreetingCard] ad_sender_begin=%d\n", r);
  if (r != AD_SEND_E_OK) { show("init failed", r); for (;;) delay(1000); }

  Serial.printf("[GreetingCard] cycle starting\n");

  int c = ad_cycle_begin(nullptr);      /* NULL = the library defaults */
  Serial.printf("[GreetingCard] ad_cycle_begin=%d\n", c);
  if (c != 0) { show("no BLE driver", c); for (;;) delay(1000); }
}

/* Pick someone to offer the card to: close, heard from just now, and not the person we
 * offered it to a moment ago. Returns nullptr when nobody qualifies, which is most of the
 * time and is the correct outcome. */
static const struct AdPeerRow *pick(const struct AdPeerTab *t, uint32_t now) {
  const struct AdPeerRow *best = nullptr;
  g_why = nullptr;
  for (int i = 0; i < ADP_ROWS; i++) {
    const struct AdPeerRow *r = &t->row[i];
    if (!r->used) continue;
    if (r->rssi_avg < NEAR_DBM) continue;      /* not near: not a candidate, not a reason */
    if (now - r->last_ms > FRESH_MS)                                  { g_why = "stale";   continue; }
    if (!memcmp(r->mac, g_last_mac, 6) && now - g_last_ms < REPEAT_MS) { g_why = "greeted"; continue; }
    if (!memcmp(r->mac, g_snub_mac, 6) && now - g_snub_ms < SNUB_MS)   { g_why = "snubbed"; continue; }
    /* strongest wins: if two people are near, the nearer one meant it */
    if (!best || r->rssi_avg > best->rssi_avg) best = r;
  }
  return best;
}

void loop() {
  M5.update();
  ad_cycle_poll();       /* BLE <-> AWDL. Never blocks; every wait is a millis() compare. */
  ad_send_poll();        /* advances a transfer by one step */

  /* The library never writes to Serial: its lines are produced on tasks the sketch does not
     own, so it stages them and you choose where they go. This example had no drain at all,
     which meant every diagnostic the library produced -- including the CYCLE lines that say
     whether the wake timing is working -- went into the ring and was overwritten unseen. */
  { char line[200];
    /* awdl_diag_read_line, NOT ad_diag_read_line. The AirDrop-layer reader belongs to the
       RECEIVER and is absent from a sender build; the ring that ad_cycle stages into is the
       AWDL layer's, and this is its reader. Using the wrong one is a link error rather than
       silence, which is the right way round. */
    while (awdl_diag_read_line(line, sizeof line)) Serial.print(line); }

  const uint32_t now = millis();
  struct AdSendStatus s; ad_send_status_read(&s);

  /* Note when a result appeared, so it can be shown for a fixed time rather than for as
     long as it happens to take the next candidate to turn up. */
  if (s.state != g_last_state) {
    g_last_state = s.state;
    if (s.state == AD_SEND_DONE || s.state == AD_SEND_FAILED) {
      g_result_ms = now;
      /* A refusal is about that device, not about the badge: remember which one so the next
         attempt goes elsewhere. A decline by a human is not a refusal by a device, so only
         the connect-level failures earn a snub. */
      if (s.state == AD_SEND_FAILED &&
          (s.err == ADS_ERR_CONNECT_REFUSED || s.err == ADS_ERR_CONNECT_TIMEOUT)) {
        memcpy(g_snub_mac, s.peer_mac, 6);
        g_snub_ms = now;
      }
    }
  }

  /* Ready for another attempt once the result has been on screen long enough to read. */
  const bool idle = (s.state == AD_SEND_IDLE) ||
                    ((s.state == AD_SEND_DONE || s.state == AD_SEND_FAILED) &&
                     now - g_result_ms >= RESULT_HOLD_MS);

  /* Look for someone only while AWDL is actually up and listening. */
  if (ad_cycle_phase() == AD_CYCLE_WATCHING && idle) {
    struct AdPeerTab tab; ad_peers_read(&tab);
    const struct AdPeerRow *p = pick(&tab, now);
    /* A send begun with less than the connect budget left does not fail faster, it fails
       later -- taking the rest of the window with it and snubbing a peer that never got a
       fair attempt. That is what "close!" followed by a timeout on the next cycle looks
       like from the outside. */
    if (p && ad_cycle_phase_left_ms() < AD_SEND_MIN_WINDOW_MS) { g_why = "no time"; p = nullptr; }
    if (p) {
      /* mtime 0 means the file lands dated 1 Jan 1970 on the receiver. A badge with no
         clock has nothing honest to put here; if yours has NTP or an RTC, pass the real
         time and the card arrives dated correctly. */
      struct AdSendFile f = { "ESP32Drop.png", CARD_PNG, CARD_PNG_LEN, 0, "image/png" };
      /* AD_SEND_HOLD stops after /Discover -- before anything appears on their screen --
         so the decision below is made while the other person is still unaware. */
      ad_send(p, &f, 1, AD_SEND_HOLD);
    }
  }

  /* /Discover has answered. This is the only place the two facts worth knowing exist:
     whether the device can receive at all, and what it is called. Deciding here means an
     unsuitable or already-greeted device never sees a prompt. */
  if (s.state == AD_SEND_HOLD_) {
    /* Only airdropable decides here. There used to be a second test -- the same NAME
       greeted recently -- and it could never fire: ad_send.h says peer_name is ALWAYS
       EMPTY in this version, because ReceiverComputerName sits in a bplist the sender has
       no reader for. So `repeat` was always false and g_last_name was always written from
       "". The cooldown that actually works is the MAC one in pick(), which is why nothing
       looked wrong.
       Bringing the name back would not be an improvement either: an AWDL MAC rotates about
       every 100 s, so a name would outlive it -- but until peer_name is real there is
       nothing to compare, and a test that cannot fire is worse than no test, because it
       reads as coverage. */
    if (!s.airdropable) {
      ad_send_abort();
    } else {
      memcpy(g_last_mac, s.peer_mac, 6);
      g_last_ms = now;
      ad_send_continue();          /* now their Accept dialog appears */
    }
  }

  draw(&s);

  /* One line per change, not one per second: an example that floods the monitor teaches the
     wrong habit, and the screen is where the person holding the badge looks anyway. */
  if (s.state != g_last_reported) {
    g_last_reported = s.state;
    Serial.printf("[GreetingCard] %s", send_line(&s) ? send_line(&s) : "watching");
    if (s.state == AD_SEND_FAILED) Serial.printf(" -- %s", fail_line(s.err));
    if (s.peer_mac[0] || s.peer_mac[1])
      Serial.printf("  %02x%02x%02x%02x%02x%02x", s.peer_mac[0], s.peer_mac[1],
                    s.peer_mac[2], s.peer_mac[3], s.peer_mac[4], s.peer_mac[5]);
    Serial.println();
  }
}
