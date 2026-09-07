/* Host tests for ad_bplist.h -- the SAME writer the firmware runs.
 *
 * Every expectation here is a byte array produced by PYTHON'S plistlib, not by this
 * writer (tools/gen_bplist_golden.sh). A golden an implementation generates for itself
 * proves only self-consistency; these come from the reference serialiser, so a byte-exact
 * match is evidence about the FORMAT rather than about our reading of the format.
 *
 * The cases cluster on the two things that are easy to get wrong and impossible to notice:
 * the length-marker escape at 15 (below it the count rides in the marker byte; at and
 * above it a 1-byte integer follows), and non-ASCII, which must switch from the 0x5 string
 * type to 0x6 UTF-16BE or become mojibake on someone else's phone.
 *
 *   cc -O2 -o /tmp/bplist_test tools/bplist_test.c && /tmp/bplist_test
 */
#include <stdio.h>
#include <string.h>
#include "../ESP32Drop/src/airdrop/core/ad_bplist.h"
#include "../testdata/bplist_golden.inc"
#include "../ESP32Drop/src/airdrop/core/airdrop_cert.h"

static int fails = 0, ran = 0;
static void check(const char *name, int ok, const char *detail) {
  ran++;
  printf("  [%s] %-56s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}

static uint8_t OUT[1024];

static void cmp(const char *name, uint32_t got_len,
                const unsigned char *want, unsigned int want_len) {
  char d[160];
  if (got_len != want_len) {
    snprintf(d, sizeof d, "length %u, expected %u", got_len, want_len);
    check(name, 0, d); return;
  }
  for (uint32_t i = 0; i < got_len; i++) {
    if (OUT[i] != want[i]) {
      snprintf(d, sizeof d, "byte %u is 0x%02x, expected 0x%02x", i, OUT[i], want[i]);
      check(name, 0, d); return;
    }
  }
  snprintf(d, sizeof d, "%u bytes, byte-exact", got_len);
  check(name, 1, d);
}

#define MODEL {"ReceiverModelName", AD_BP_STR, "M5Stack", 0}
#define NAME(s) {"ReceiverComputerName", AD_BP_STR, (s), 0}

int main(void) {
  puts("== ad_bplist.h vs Python plistlib ==");

  /* The two responses the firmware actually returns. */
  { struct AdBplistKV kv[] = {
      NAME("M5 Badge"),
      {"ReceiverMediaCapabilities", AD_BP_DATA, "{\"Version\":1}", 13},
      MODEL };
    cmp("/Discover response, the real one", ad_bplist_dict(OUT, sizeof OUT, kv, 3),
        BP_discover_m5, BP_discover_m5_len); }

  { struct AdBplistKV kv[] = { NAME("M5 Badge"), MODEL };
    cmp("/Ask response, the real one", ad_bplist_dict(OUT, sizeof OUT, kv, 2),
        BP_ask_m5, BP_ask_m5_len); }

  /* The marker escape. Below 15 the count is in the low nibble; at 15 and above a
     0x10 1-byte integer follows. Off by one here and every offset after it shifts. */
  { struct AdBplistKV kv[] = { NAME("AAAAAAAAAAAAAA"), MODEL };          /* 14 */
    cmp("a 14-character name (count rides in the marker)", ad_bplist_dict(OUT, sizeof OUT, kv, 2),
        BP_name_len14, BP_name_len14_len); }
  { struct AdBplistKV kv[] = { NAME("AAAAAAAAAAAAAAA"), MODEL };         /* 15 */
    cmp("a 15-character name (the escape boundary)", ad_bplist_dict(OUT, sizeof OUT, kv, 2),
        BP_name_len15, BP_name_len15_len); }
  { struct AdBplistKV kv[] = { NAME("AAAAAAAAAAAAAAAA"), MODEL };        /* 16 */
    cmp("a 16-character name", ad_bplist_dict(OUT, sizeof OUT, kv, 2),
        BP_name_len16, BP_name_len16_len); }
  { struct AdBplistKV kv[] = { NAME(""), MODEL };
    cmp("an empty name is a valid plist, not a refusal", ad_bplist_dict(OUT, sizeof OUT, kv, 2),
        BP_name_empty, BP_name_empty_len); }

  /* Non-ASCII. bplist's 0x5 string type cannot carry these at all. */
  { struct AdBplistKV kv[] = { NAME("\xe3\x83\xaa\xe3\x83\x93\xe3\x83\xb3\xe3\x82\xb0\xe3\x81\xae\xe3\x83\x90\xe3\x83\x83\xe3\x82\xb8"), MODEL };
    cmp("a Japanese name becomes UTF-16BE", ad_bplist_dict(OUT, sizeof OUT, kv, 2),
        BP_name_ja, BP_name_ja_len); }
  { struct AdBplistKV kv[] = { NAME("\xe3\x81\x82\xe3\x81\x82\xe3\x81\x82\xe3\x81\x82\xe3\x81\x82\xe3\x81\x82\xe3\x81\x82\xe3\x81\x82\xe3\x81\x82\xe3\x81\x82\xe3\x81\x82\xe3\x81\x82\xe3\x81\x82\xe3\x81\x82\xe3\x81\x82"), MODEL };
    cmp("15 Japanese characters -- 15 UNITS, not 45 bytes", ad_bplist_dict(OUT, sizeof OUT, kv, 2),
        BP_name_ja_long, BP_name_ja_long_len); }
  { struct AdBplistKV kv[] = { NAME("badge \xf0\x9f\x90\xb1"), MODEL };
    cmp("an emoji is a surrogate PAIR: two units for one character",
        ad_bplist_dict(OUT, sizeof OUT, kv, 2), BP_name_emoji, BP_name_emoji_len); }

  { struct AdBplistKV kv[] = { NAME("solo") };
    cmp("a one-pair dictionary", ad_bplist_dict(OUT, sizeof OUT, kv, 1),
        BP_one_pair, BP_one_pair_len); }

  /* --- integers, for the SEND path -----------------------------------------------
   *
   * A real macOS sender's /Discover request body is
   *   {SenderRecordData: <data 3802>, DeviceSupportFlags: 111611}
   * measured off the air, so the writer has to be
   * able to emit an integer at all. plistlib picks the SMALLEST width that holds the
   * value; a receiver that byte-compares would see a different file if we picked another,
   * which is the same reason the keys are sorted here. These cases sit on both sides of
   * each width boundary, because "works for 111611" would not have caught a wrong
   * threshold. */
#define DSF(n) {"DeviceSupportFlags", AD_BP_INT, NULL, 0, (n)}
  { struct AdBplistKV kv[] = { DSF(111611) };
    cmp("DeviceSupportFlags=111611, the value a real macOS sender sends",
        ad_bplist_dict(OUT, sizeof OUT, kv, 1), BP_int_dsf, BP_int_dsf_len); }
  { struct AdBplistKV kv[] = { DSF(0) };
    cmp("zero is one byte", ad_bplist_dict(OUT, sizeof OUT, kv, 1),
        BP_int_0, BP_int_0_len); }
  { struct AdBplistKV kv[] = { DSF(255) };
    cmp("255 is still one byte", ad_bplist_dict(OUT, sizeof OUT, kv, 1),
        BP_int_255, BP_int_255_len); }
  { struct AdBplistKV kv[] = { DSF(256) };
    cmp("256 crosses to two", ad_bplist_dict(OUT, sizeof OUT, kv, 1),
        BP_int_256, BP_int_256_len); }
  { struct AdBplistKV kv[] = { DSF(65535) };
    cmp("65535 is still two", ad_bplist_dict(OUT, sizeof OUT, kv, 1),
        BP_int_65535, BP_int_65535_len); }
  { struct AdBplistKV kv[] = { DSF(65536) };
    cmp("65536 crosses to four", ad_bplist_dict(OUT, sizeof OUT, kv, 1),
        BP_int_65536, BP_int_65536_len); }
  { struct AdBplistKV kv[] = { DSF(111611),
      {"SenderRecordData", AD_BP_DATA, "\x30\x80\x06\x09", 4} };
    cmp("an integer beside a data value, keys sorted",
        ad_bplist_dict(OUT, sizeof OUT, kv, 2), BP_int_mixed, BP_int_mixed_len); }
  /* Negative values are refused rather than guessed at: Apple always writes them as 8
     bytes and nothing here needs one, so an untested encoding never reaches the wire. */
  { struct AdBplistKV kv[] = { DSF(-1) };
    check("a negative integer is refused, not guessed at",
          ad_bplist_dict(OUT, sizeof OUT, kv, 1) == 0, "returns 0"); }

  /* --- the NESTED writer (ad_bpw_*) -------------------------------------------------
   *
   * Two things here are not guessable from the format and are the whole reason these
   * goldens exist: plistlib's flatten ORDER (the container, then ALL KEYS, then ALL
   * VALUES) and its DEDUPLICATION of identical scalars. Get either wrong and every ref
   * downstream of the first container shifts, which moves the offset table, which moves
   * the trailer -- and nothing says so except a byte comparison. */
  { /* the /Ask shape: a dict whose Files is an ARRAY OF DICTS. The flat writer cannot
       express this at all, which is why this writer exists. */
    struct AdBpw w; ad_bpw_init(&w);
    int root = ad_bpw_dict(&w);
    int f    = ad_bpw_dict(&w);
    ad_bpw_set(&w, f, "FileName", ad_bpw_str(&w, "card.png"));
    ad_bpw_set(&w, f, "FileType", ad_bpw_str(&w, "public.png"));
    ad_bpw_set(&w, f, "FileBomPath", ad_bpw_str(&w, "./card.png"));
    ad_bpw_set(&w, f, "FileIsDirectory", ad_bpw_bool(&w, false));
    ad_bpw_set(&w, f, "ConvertMediaFormats", ad_bpw_bool(&w, false));
    int arr = ad_bpw_array(&w);
    ad_bpw_push(&w, arr, f);
    ad_bpw_set(&w, root, "TransferID", ad_bpw_str(&w, "7246C554-E5C8-47C8-B9A5-F5CECF2FAF95"));
    ad_bpw_set(&w, root, "SenderComputerName", ad_bpw_str(&w, "M5 Badge"));
    ad_bpw_set(&w, root, "Files", arr);
    cmp("an /Ask-shaped body: Files is an array of dicts",
        ad_bpw_finish(&w, root, OUT, sizeof OUT), BP_nest_ask, BP_nest_ask_len); }

  { /* the same string three times must be ONE object referenced three times */
    struct AdBpw w; ad_bpw_init(&w);
    int root = ad_bpw_dict(&w);
    int arr = ad_bpw_array(&w);
    ad_bpw_push(&w, arr, ad_bpw_str(&w, "same"));
    ad_bpw_push(&w, arr, ad_bpw_str(&w, "other"));
    ad_bpw_set(&w, root, "A", ad_bpw_str(&w, "same"));
    ad_bpw_set(&w, root, "B", ad_bpw_str(&w, "same"));
    ad_bpw_set(&w, root, "C", arr);
    cmp("an identical scalar is ONE object, as plistlib deduplicates it",
        ad_bpw_finish(&w, root, OUT, sizeof OUT), BP_nest_dedup, BP_nest_dedup_len); }

  { struct AdBpw w; ad_bpw_init(&w);
    int root = ad_bpw_dict(&w);
    ad_bpw_set(&w, root, "T", ad_bpw_bool(&w, true));
    ad_bpw_set(&w, root, "F", ad_bpw_bool(&w, false));
    cmp("true is 0x09 and false is 0x08", ad_bpw_finish(&w, root, OUT, sizeof OUT),
        BP_nest_bool, BP_nest_bool_len); }

  { struct AdBpw w; ad_bpw_init(&w);
    int root = ad_bpw_dict(&w), arr = ad_bpw_array(&w);
    ad_bpw_push(&w, arr, ad_bpw_int(&w, 1));
    ad_bpw_push(&w, arr, ad_bpw_int(&w, 2));
    ad_bpw_push(&w, arr, ad_bpw_int(&w, 300));
    ad_bpw_push(&w, arr, ad_bpw_int(&w, 70000));
    ad_bpw_set(&w, root, "Items", arr);
    cmp("an array of integers across all three widths",
        ad_bpw_finish(&w, root, OUT, sizeof OUT), BP_nest_ints, BP_nest_ints_len); }

  { struct AdBpw w; ad_bpw_init(&w);
    int l3 = ad_bpw_dict(&w); ad_bpw_set(&w, l3, "L3", ad_bpw_str(&w, "leaf"));
    int l2 = ad_bpw_dict(&w); ad_bpw_set(&w, l2, "L2", l3);
    int l1 = ad_bpw_dict(&w); ad_bpw_set(&w, l1, "L1", l2);
    cmp("three levels of dictionary", ad_bpw_finish(&w, l1, OUT, sizeof OUT),
        BP_nest_deep, BP_nest_deep_len); }

  { /* two dicts with the SAME key: the key is one object, the dicts are two */
    struct AdBpw w; ad_bpw_init(&w);
    int a = ad_bpw_dict(&w); ad_bpw_set(&w, a, "FileName", ad_bpw_str(&w, "a.png"));
    int b = ad_bpw_dict(&w); ad_bpw_set(&w, b, "FileName", ad_bpw_str(&w, "b.png"));
    int arr = ad_bpw_array(&w); ad_bpw_push(&w, arr, a); ad_bpw_push(&w, arr, b);
    int root = ad_bpw_dict(&w); ad_bpw_set(&w, root, "Files", arr);
    cmp("two file entries share their key object but not their dicts",
        ad_bpw_finish(&w, root, OUT, sizeof OUT), BP_nest_two_files, BP_nest_two_files_len); }

  { /* UTF-16 inside a nested value, not just at the top level */
    struct AdBpw w; ad_bpw_init(&w);
    int f = ad_bpw_dict(&w);
    ad_bpw_set(&w, f, "FileName", ad_bpw_str(&w, "\xe3\x82\xab\xe3\x83\xbc\xe3\x83\x89.png"));
    int arr = ad_bpw_array(&w); ad_bpw_push(&w, arr, f);
    int root = ad_bpw_dict(&w); ad_bpw_set(&w, root, "Files", arr);
    cmp("a Japanese filename nested two levels down",
        ad_bpw_finish(&w, root, OUT, sizeof OUT), BP_nest_ja, BP_nest_ja_len); }

  { /* the pool has to refuse, not overflow */
    struct AdBpw w; ad_bpw_init(&w);
    int root = ad_bpw_dict(&w);
    for (int i = 0; i < AD_BPW_KIDS; i++) ad_bpw_set(&w, root, "k", ad_bpw_int(&w, i));
    check("outgrowing the node pool is a refusal, not a smashed stack",
          ad_bpw_finish(&w, root, OUT, sizeof OUT) == 0, "returns 0"); }

  { /* two containers filled alternately: kid0/nkid is a slice, so this must refuse */
    struct AdBpw w; ad_bpw_init(&w);
    int a = ad_bpw_array(&w), b = ad_bpw_array(&w);
    ad_bpw_push(&w, a, ad_bpw_int(&w, 1));
    ad_bpw_push(&w, b, ad_bpw_int(&w, 2));
    ad_bpw_push(&w, a, ad_bpw_int(&w, 3));       /* a is no longer consecutive */
    int root = ad_bpw_dict(&w); ad_bpw_set(&w, root, "A", a);
    check("interleaving two open containers is refused, not silently merged",
          ad_bpw_finish(&w, root, OUT, sizeof OUT) == 0, "returns 0"); }

  /* Keys are sorted by the writer, because plistlib sorts them and a caller cannot be
     expected to know that. Same input, shuffled, must give the same bytes. */
  { struct AdBplistKV kv[] = { MODEL,
      {"ReceiverMediaCapabilities", AD_BP_DATA, "{\"Version\":1}", 13},
      NAME("M5 Badge") };
    cmp("keys out of order produce identical bytes", ad_bplist_dict(OUT, sizeof OUT, kv, 3),
        BP_discover_m5, BP_discover_m5_len); }

  /* THE REGRESSION THAT MATTERS. The two responses used to be 156- and 110-byte constants
     compiled into the firmware, and those constants are what a real Mac has been accepting
     for the life of this project. If the writer produces the same bytes for the same name,
     nothing on the wire changed and the runtime build is a pure refactor. */
  puts("== the constants this replaces ==");
  { struct AdBplistKV kv[] = {
      NAME("M5 Badge"),
      {"ReceiverMediaCapabilities", AD_BP_DATA, "{\"Version\":1}", 13},
      MODEL };
    cmp("built /Discover == the constant a Mac has been accepting",
        ad_bplist_dict(OUT, sizeof OUT, kv, 3),
        AIRDROP_DISCOVER_BPLIST, AIRDROP_DISCOVER_BPLIST_LEN); }
  { struct AdBplistKV kv[] = { NAME("M5 Badge"), MODEL };
    cmp("built /Ask == the constant a Mac has been accepting",
        ad_bplist_dict(OUT, sizeof OUT, kv, 2),
        AIRDROP_ASK_BPLIST, AIRDROP_ASK_BPLIST_LEN); }

  puts("== refusals ==");
  { struct AdBplistKV kv[] = { NAME("x"), MODEL };
    check("no room is a refusal, not a truncated plist",
          ad_bplist_dict(OUT, 32, kv, 2) == 0, NULL);
    check("zero pairs is a refusal", ad_bplist_dict(OUT, sizeof OUT, kv, 0) == 0, NULL);
    check("more pairs than supported is a refusal",
          ad_bplist_dict(OUT, sizeof OUT, kv, AD_BP_MAX_PAIRS + 1) == 0, NULL); }
  { struct AdBplistKV kv[] = { {"k", AD_BP_STR, NULL, 0} };
    check("a null value is a refusal, not a crash", ad_bplist_dict(OUT, sizeof OUT, kv, 1) == 0, NULL); }
  { /* A lone continuation byte: malformed UTF-8 is REFUSED rather than replaced. A name
       is something a user typed; quietly turning half of it into U+FFFD is worse. */
    struct AdBplistKV kv[] = { {"ReceiverComputerName", AD_BP_STR, "\x80\x80", 2}, MODEL };
    check("malformed UTF-8 is refused, not substituted",
          ad_bplist_dict(OUT, sizeof OUT, kv, 2) == 0, NULL); }
  { struct AdBplistKV kv[] = { {"ReceiverComputerName", AD_BP_STR, "\xe3\x83", 2}, MODEL };
    check("a truncated UTF-8 sequence is refused",
          ad_bplist_dict(OUT, sizeof OUT, kv, 2) == 0, NULL); }
  { char big[300]; memset(big, 'A', sizeof big - 1); big[sizeof big - 1] = 0;
    struct AdBplistKV kv[] = { NAME(big), MODEL };
    check("a name longer than one length byte is refused, not silently cut",
          ad_bplist_dict(OUT, sizeof OUT, kv, 2) == 0, NULL); }

  printf("\n%d checks, %d failed\n", ran, fails);
  return fails ? 1 : 0;
}
