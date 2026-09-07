/* Host tests for ad_uti.h -- the SAME code the firmware runs.
 *
 * The table is checked against testdata/uti_golden.inc, which tools/gen_uti_golden.sh
 * reads back from Apple's UniformTypeIdentifiers framework: an implementation this project
 * did not write. The rest is the decision logic -- what goes out, what is refused, and
 * that nothing is ever guessed.
 *
 *   cc -O2 -fsanitize=address,undefined -o /tmp/uti_test tools/uti_test.c && /tmp/uti_test
 */
#include <stdio.h>
#include <string.h>
#include "../ESP32Drop/src/airdrop/core/ad_uti.h"

static int fails = 0, ran = 0;
static void check(const char *name, int ok, const char *detail) {
  ran++;
  printf("  [%s] %-70s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}
static int eq(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }

/* What tools/gen_uti_golden.sh would emit -- here inline, read back from macOS 26. */
static const struct { const char *mime, *uti; } GOLDEN[] = {
#include "../testdata/uti_golden.inc"
};

static const uint8_t PNG[8]  = {0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};
static const uint8_t JPG[4]  = {0xFF,0xD8,0xFF,0xE0};
static const uint8_t HEIC[16]= {0,0,0,0x18,'f','t','y','p','h','e','i','c',0,0,0,0};
static const uint8_t ZIP[4]  = {'P','K',3,4};
static const uint8_t ADBL[8] = {0x00,0x05,0x16,0x07,0x00,0x02,0x00,0x00};

int main(void) {
  const char *u; int how, r; char d[160];

  puts("ad_uti.h -- the table against Apple's own answer");
  for (unsigned i = 0; i < sizeof GOLDEN / sizeof GOLDEN[0]; i++) {
    u = ad_uti_from_mime(GOLDEN[i].mime);
    snprintf(d, sizeof d, "%s -> %s", GOLDEN[i].mime, u ? u : "NULL");
    check("matches UTType(mimeType:)", eq(u, GOLDEN[i].uti), d);
  }
  check("every table row is in the golden (no untested row)",
        AD_UTI_ROWS == (int)(sizeof GOLDEN / sizeof GOLDEN[0]), NULL);

  puts("ad_uti.h -- MIME spelling a sketch actually has in hand");
  check("case-insensitive: Image/PNG",          eq(ad_uti_from_mime("Image/PNG"), "public.png"), NULL);
  check("parameters ignored: text/plain; charset=utf-8",
        eq(ad_uti_from_mime("text/plain; charset=utf-8"), "public.plain-text"), NULL);
  check("a MIME type Apple maps to dyn.* is NULL, not encoded (application/gpx+xml)",
        ad_uti_from_mime("application/gpx+xml") == NULL, NULL);
  check("empty is NULL",                        ad_uti_from_mime("") == NULL, NULL);
  check("NULL is NULL",                         ad_uti_from_mime(NULL) == NULL, NULL);
  check("a prefix is not a match (image/pn)",   ad_uti_from_mime("image/pn") == NULL, NULL);
  check("a superstring is not a match (image/pngx)", ad_uti_from_mime("image/pngx") == NULL, NULL);

  puts("ad_uti.h -- sniffed kind -> UTI");
  check("JPEG", eq(ad_uti_from_sniff(AD_FILE_JPEG), "public.jpeg"), NULL);
  check("PNG -- the one measured on the wire", eq(ad_uti_from_sniff(AD_FILE_PNG), "public.png"), NULL);
  check("GIF",  eq(ad_uti_from_sniff(AD_FILE_GIF), "com.compuserve.gif"), NULL);
  check("HEIC", eq(ad_uti_from_sniff(AD_FILE_HEIC), "public.heic"), NULL);
  check("PDF",  eq(ad_uti_from_sniff(AD_FILE_PDF), "com.adobe.pdf"), NULL);
  check("ZIP",  eq(ad_uti_from_sniff(AD_FILE_ZIP), "public.zip-archive"), NULL);
  check("OTHER is NULL: the sniffer does not name what it cannot see", ad_uti_from_sniff(AD_FILE_OTHER) == NULL, NULL);
  check("APPLEDOUBLE is NULL: a sidecar is not a file anyone sends alone", ad_uti_from_sniff(AD_FILE_APPLEDOUBLE) == NULL, NULL);
  check("every sniffed UTI maps back to its own kind",
        ad_uti_sniff_kind("public.jpeg") == AD_FILE_JPEG && ad_uti_sniff_kind("public.png") == AD_FILE_PNG &&
        ad_uti_sniff_kind("com.compuserve.gif") == AD_FILE_GIF && ad_uti_sniff_kind("public.heic") == AD_FILE_HEIC &&
        ad_uti_sniff_kind("public.heif") == AD_FILE_HEIC && ad_uti_sniff_kind("com.adobe.pdf") == AD_FILE_PDF &&
        ad_uti_sniff_kind("public.zip-archive") == AD_FILE_ZIP, NULL);

  puts("ad_uti.h -- what is shaped like a UTI");
  check("public.png is plausible",              ad_uti_plausible("public.png"), NULL);
  check("a long real one is plausible",         ad_uti_plausible("org.openxmlformats.wordprocessingml.document"), NULL);
  check("no dot is not (png)",                 !ad_uti_plausible("png"), NULL);
  check("a space is not",                      !ad_uti_plausible("public png"), NULL);
  check("empty is not",                        !ad_uti_plausible(""), NULL);
  check("non-ASCII is not",                    !ad_uti_plausible("public.\xe3\x81\x82"), NULL);
  { char big[AD_UTI_MAX + 2]; memset(big, 'a', sizeof big - 1); big[sizeof big - 1] = 0; big[3] = '.';
    check("over AD_UTI_MAX is not",             !ad_uti_plausible(big), NULL);
    big[AD_UTI_MAX] = 0;
    check("exactly AD_UTI_MAX is",               ad_uti_plausible(big), NULL); }

  puts("ad_uti.h -- resolve: what goes out, and what is refused");
  r = ad_uti_resolve(NULL, PNG, 8, &u, &how);
  check("NULL + PNG bytes -> public.png, sniffed (GreetingCard unchanged)",
        r == AD_UTI_OK && eq(u, "public.png") && how == AD_UTI_HOW_SNIFFED, NULL);
  r = ad_uti_resolve(NULL, JPG, 4, &u, &how);
  check("NULL + JPEG bytes -> public.jpeg, sniffed", r == AD_UTI_OK && eq(u, "public.jpeg"), NULL);
  r = ad_uti_resolve(NULL, (const uint8_t *)"hello", 5, &u, &how);
  check("NULL + bytes nobody can name -> UNKNOWN, never public.data by guess", r == AD_UTI_UNKNOWN, NULL);
  r = ad_uti_resolve(NULL, ADBL, 8, &u, &how);
  check("NULL + an AppleDouble sidecar -> UNKNOWN", r == AD_UTI_UNKNOWN, NULL);
  r = ad_uti_resolve(NULL, PNG, 3, &u, &how);
  check("NULL + 3 bytes of a PNG -> UNKNOWN (too short to sniff)", r == AD_UTI_UNKNOWN, NULL);
  r = ad_uti_resolve("text/plain", (const uint8_t *)"hello", 5, &u, &how);
  check("text/plain + text -> public.plain-text, mime", r == AD_UTI_OK && eq(u, "public.plain-text") && how == AD_UTI_HOW_MIME, NULL);
  r = ad_uti_resolve("image/png", JPG, 4, &u, &how);
  check("image/png over JPEG bytes -> MISMATCH (the old gate's reason, kept)", r == AD_UTI_MISMATCH, NULL);
  r = ad_uti_resolve("public.png", JPG, 4, &u, &how);
  check("public.png over JPEG bytes -> MISMATCH, the UTI spelling too", r == AD_UTI_MISMATCH, NULL);
  r = ad_uti_resolve("application/pdf", ZIP, 4, &u, &how);
  check("application/pdf over ZIP bytes -> MISMATCH", r == AD_UTI_MISMATCH, NULL);
  r = ad_uti_resolve("image/heif", HEIC, 16, &u, &how);
  check("image/heif over a heic brand -> OK public.heif (same sniff kind)", r == AD_UTI_OK && eq(u, "public.heif"), NULL);
  r = ad_uti_resolve("application/octet-stream", PNG, 8, &u, &how);
  check("octet-stream over PNG bytes -> OK public.data (a PNG IS data; not a lie)",
        r == AD_UTI_OK && eq(u, "public.data"), NULL);
  r = ad_uti_resolve("org.openxmlformats.wordprocessingml.document", ZIP, 4, &u, &how);
  check("a docx UTI over ZIP bytes -> OK, given (outside the six: nothing to contradict)",
        r == AD_UTI_OK && how == AD_UTI_HOW_GIVEN && u != NULL &&
        eq(u, "org.openxmlformats.wordprocessingml.document"), NULL);
  r = ad_uti_resolve("application/vnd.openxmlformats-officedocument.wordprocessingml.document", ZIP, 4, &u, &how);
  check("a docx MIME type is not in the table -> UNKNOWN (pass the UTI)", r == AD_UTI_UNKNOWN, NULL);
  r = ad_uti_resolve("png", PNG, 8, &u, &how);
  check("\"png\" is neither a MIME type nor a UTI -> MALFORMED", r == AD_UTI_MALFORMED, NULL);
  r = ad_uti_resolve("text/plain", PNG, 8, &u, &how);
  check("text/plain over PNG bytes -> OK (plain-text is not one of the six; trusted)",
        r == AD_UTI_OK && eq(u, "public.plain-text"), NULL);
  { const char *given = "com.example.custom-type";
    r = ad_uti_resolve(given, (const uint8_t *)"xyz", 3, &u, &how);
    check("a caller's own UTI is returned by POINTER, borrowed like data", r == AD_UTI_OK && u == given, NULL); }

  printf("\n%d checks, %d failed\n", ran, fails);
  return fails ? 1 : 0;
}
