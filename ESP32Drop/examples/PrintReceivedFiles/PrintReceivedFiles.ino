/* PrintReceivedFiles -- the smallest thing that receives an AirDrop transfer.
 *
 * No display, no SD card, no libraries beyond this one. Send a file from a Mac or iPhone
 * and the name, size and type appear on the serial monitor. Start here to see the whole
 * API; ShowReceivedImage does something with the bytes.
 *
 * NEEDS an ESP32-S3 with PSRAM -- transfers are decoded there.
 * TAKES THE RADIO EXCLUSIVELY -- do not also call WiFi.begin().
 * NEEDS NO PAIRING, no Apple ID, no network, no router. A phone that has never met this
 * device can send to it.
 */
#include <ESP32AWDL.h>
#include <ESP32Drop.h>

static bool on_file(void *, const struct AdFile *f) {
  Serial.printf("[%lu/%lu] %-28s %8lu bytes  %s\n",
                (unsigned long)f->index + 1, (unsigned long)f->count,
                f->name, (unsigned long)f->len, ad_cpio_type_name(f->type));

  /* f->data is borrowed and stops being valid when this returns -- copy what you keep.
     Return false for a file you could not use; it is counted in ad_stats_read() rather
     than disappearing. Here everything is "used", because printing always works. */
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  /* AWDL first: the AirDrop listener binds to the address that interface owns. */
  if (awdl_begin() != ESP_OK) { Serial.println("AWDL failed"); return; }

  ad_on_file(on_file, nullptr);

  /* Second argument bounds the decoded archive we accept, and with it the PSRAM this
     library holds during a transfer. 0 = the library default of 6 MiB. Over the limit the
     sender gets HTTP 413, so it reports a failure instead of a silent success. */
  if (ad_begin("ESP32 Receiver", 0) != 0) { Serial.println("AirDrop failed"); return; }

  Serial.println("ready -- look for \"ESP32 Receiver\" in the AirDrop list");
}

void loop() {
  ad_poll();          /* runs on_file() on THIS task; cheap when idle */

  /* The library never touches Serial itself -- its diagnostics are produced on the TLS
     task, so it stages them and you choose where they go. */
  char line[200];
  while (ad_diag_read_line(line, sizeof(line))) Serial.print(line);

  delay(10);
}
