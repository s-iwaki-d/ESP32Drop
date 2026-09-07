/* ShowReceivedImage -- receive a photo over AirDrop and put it on the screen.
 *
 * Send a picture to this device from a Mac or iPhone: open the share sheet, pick AirDrop,
 * and the name below will be in the list. The photo appears on the display.
 *
 * WHAT YOU NEED
 *   An ESP32-S3 with PSRAM. A transfer is decoded in PSRAM, so a part without it cannot
 *   receive a photo of any size. This sketch was measured on an M5Stack StopWatch.
 *
 * WHAT THIS TAKES OVER
 *   The radio, exclusively. AWDL parks on channel 6 and keeps a transmit schedule locked
 *   to the mesh's availability window, so it cannot share the interface with WiFi.begin().
 *   Do not call both.
 *
 * WHAT IT DOES NOT NEED
 *   Pairing, an Apple ID, a network, or a router. AWDL is a direct link between the
 *   devices. A phone that has never met this one can send to it.
 *
 * BLOCKING IS FINE. The transmit cadence owns its own task, so loop() may block for as
 * long as you like without the device losing its place in the mesh -- measured at 100% of
 * the window ceiling with a one-second block in loop().
 */
#include <M5Unified.h>
#include <ESP32AWDL.h>
#include <ESP32Drop.h>

/* The name the sender's share sheet shows. UTF-8, so Japanese and emoji work. */
static const char *BADGE_NAME = "M5 Badge";

/* How large a transfer to accept, in bytes of decoded archive. This also bounds the PSRAM
 * the library holds while a transfer is running, so it is worth setting deliberately: 0
 * means the library's default of 6 MiB; pass a larger value if you want more (an 8 MB
 * part can hold about 7.1 MB of decoded archive).
 * Anything larger is refused with HTTP 413, and the sender is told the transfer failed
 * rather than left believing it worked. A 4 MB photo decodes to about 3.9 MB. */
static const uint32_t MAX_RECEIVE = 0;

/* Called once per file in a transfer, on the task that calls ad_poll() -- so drawing,
 * writing to an SD card and blocking are all allowed here.
 *
 * `f->data` is borrowed: it stops being valid when this returns. Copy anything you want to
 * keep. Return false if you could not use the file; that is counted and reported through
 * ad_stats_read(), which is how a device that receives a format it cannot draw says so
 * instead of failing silently. */
static bool on_file(void *, const struct AdFile *f) {
  Serial.printf("got %s (%lu bytes, %s)\n", f->name, (unsigned long)f->len,
                ad_cpio_type_name(f->type));

  if (f->type == AD_FILE_JPEG || f->type == AD_FILE_PNG) {
    M5.Display.fillScreen(TFT_BLACK);
    /* The last argument centres and scales to fit. drawJpg/drawPng decode straight from
       the buffer -- no temporary file, no second copy in PSRAM. */
    bool ok = (f->type == AD_FILE_JPEG)
      ? M5.Display.drawJpg(f->data, f->len, 0, 0, 0, 0, 0, 0, 0.0f, 0.0f, middle_center)
      : M5.Display.drawPng(f->data, f->len, 0, 0, 0, 0, 0, 0, 0.0f, 0.0f, middle_center);
    return ok;
  }

  /* Every transfer from a Mac also carries an AppleDouble sidecar (a "._name" file) with
     the Finder's metadata in it. There is nothing to show and nothing went wrong, so it is
     accepted rather than counted as an error. */
  if (f->type == AD_FILE_APPLEDOUBLE) return true;

  return false;   /* something this sketch cannot draw */
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString("starting AWDL...", M5.Display.width() / 2,
                        M5.Display.height() / 2);

  /* Bring up the link first: the AirDrop listener binds to the address the AWDL interface
     owns, so it has to exist. This takes a moment -- the device has to hear a nearby Apple
     device announce itself before it knows where the availability window is. */
  if (awdl_begin() != ESP_OK) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.drawString("AWDL failed", M5.Display.width() / 2, M5.Display.height() / 2);
    return;
  }

  ad_on_file(on_file, nullptr);
  if (ad_begin(BADGE_NAME, MAX_RECEIVE) != 0) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.drawString("AirDrop failed", M5.Display.width() / 2, M5.Display.height() / 2);
    return;
  }
  Serial.printf("ready -- look for \"%s\" in the AirDrop list\n", BADGE_NAME);
}

void loop() {
  M5.update();

  /* The only thing this sketch must do regularly. ad_poll() runs on_file() on THIS task and
     releases the library's buffer when it returns; it is cheap when there is nothing to
     hand over. Everything on the air happens on the library's own tasks. */
  ad_poll();

  /* The library never writes to Serial. It stages diagnostic lines and you decide where
     they go -- which is the only arrangement that works, since they are produced on the
     TLS task. Delete this loop and the library goes quiet. */
  char line[200];
  while (ad_diag_read_line(line, sizeof(line))) Serial.print(line);

  /* Show progress while a transfer is running. rx_bytes is compressed bytes received; a
     photo's archive is about the same size. */
  static bool showing = false;
  struct AdStatus st; ad_stats_read(&st);
  if (st.rx_active) {
    if (!showing) { M5.Display.fillScreen(TFT_BLACK); showing = true; }
    char msg[32]; snprintf(msg, sizeof(msg), "receiving %lu KB", (unsigned long)(st.rx_bytes / 1024));
    M5.Display.drawString(msg, M5.Display.width() / 2, M5.Display.height() / 2);
  } else {
    showing = false;
  }

  delay(10);
}
