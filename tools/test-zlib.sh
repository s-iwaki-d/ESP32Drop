#!/usr/bin/env bash
# Host tests for the SEND path's compressor-free zlib emitter (ad_zlib.h), compiled
# unchanged from the firmware source -- and cross-checked against a REAL zlib.
#
# Two halves, and both are needed:
#   1. zlib_test.c pins the byte layout field by field AND checks the writer against
#      dvzip_complete() in awdl_http.h, the reader that decides whether an upload is
#      finished. A writer tested only against my own assumptions would agree with
#      itself and still hang a transfer.
#   2. The python pass feeds every emitted stream to a real zlib through BOTH the
#      one-shot and the streaming API. The streaming API is what a receiver uses and
#      is stricter about where a stream ends, so "python zlib accepted it" is the
#      claim that actually transfers to macOS.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

cc -O2 -Wall -Wextra -Wno-unused-function -o /tmp/zlib_test tools/zlib_test.c
/tmp/zlib_test
rc=$?

echo
echo "-- cross-check against a real zlib --"
cat > /tmp/zlib_emit.c <<'CEOF'
#include <stdio.h>
#include <stdlib.h>
#include "ad_zlib.h"
int main(int argc, char **argv) { (void)argc;
  uint32_t n = (uint32_t)strtoul(argv[1], 0, 10);
  uint8_t *in = malloc(n ? n : 1);
  for (uint32_t i = 0; i < n; i++) in[i] = (uint8_t)(i * 31 + (i >> 8));
  uint32_t cap = adz_bound(n); uint8_t *out = malloc(cap);
  uint32_t m = adz_deflate_stored(out, cap, in, n);
  FILE *f = fopen(argv[2], "wb"); fwrite(out, 1, m, f); fclose(f);
  f = fopen(argv[3], "wb"); if (n) fwrite(in, 1, n, f); fclose(f);
  return m ? 0 : 1;
}
CEOF
cc -O2 -Wall -Wextra -Wno-unused-function -I"$ROOT/ESP32Drop/src/airdrop/core" -o /tmp/zlib_emit /tmp/zlib_emit.c
for n in 0 1 100 65534 65535 65536 131070 200000; do
  /tmp/zlib_emit "$n" "/tmp/zt_$n.z" "/tmp/zt_$n.raw"
done
python3 - <<'PEOF' || rc=1
import zlib
ok = True
for n in (0, 1, 100, 65534, 65535, 65536, 131070, 200000):
    comp = open(f"/tmp/zt_{n}.z", "rb").read()
    raw  = open(f"/tmp/zt_{n}.raw", "rb").read()
    try:
        one = zlib.decompress(comp)
        d   = zlib.decompressobj()
        stream = d.decompress(comp) + d.flush()
        good = one == raw and stream == raw and not d.unused_data
    except Exception as e:
        good, one = False, e
    print(f"  [{'PASS' if good else 'FAIL'}] real zlib round-trip"
          f"{'':<34} n={n} bytes={len(comp)}")
    ok = ok and good
print(f"\nreal-zlib cross-check: {'all sizes OK' if ok else 'FAILURES PRESENT'}")
raise SystemExit(0 if ok else 1)
PEOF

echo
echo "-- multi-record body vs a real zlib (the /Upload grain split) --"
# The macOS-relevant claim: a body cut into grain records, when each record is
# inflated independently by a REAL zlib and concatenated, reproduces the input exactly.
cat > /tmp/zlib_body_emit.c <<'CEOF'
#include <stdio.h>
#include <stdlib.h>
#include "ad_zlib.h"
int main(int argc, char **argv) { (void)argc;
  uint32_t n = (uint32_t)strtoul(argv[1], 0, 10);
  uint32_t grain = (uint32_t)strtoul(argv[2], 0, 10);
  uint8_t *in = malloc(n ? n : 1);
  for (uint32_t i = 0; i < n; i++) in[i] = (uint8_t)(i * 31 + (i >> 8));
  uint32_t cap = adz_dvzip_body_bound(n, grain); uint8_t *out = malloc(cap);
  uint32_t m = adz_dvzip_body(out, cap, in, n, grain);
  FILE *f = fopen(argv[3], "wb"); fwrite(out, 1, m, f); fclose(f);
  f = fopen(argv[4], "wb"); if (n) fwrite(in, 1, n, f); fclose(f);
  return m ? 0 : 1;
}
CEOF
cc -O2 -Wall -Wextra -Wno-unused-function -I"$ROOT/ESP32Drop/src/airdrop/core" -o /tmp/zlib_body_emit /tmp/zlib_body_emit.c
# grain 65536 so modest sizes still make several records; the last case spans 5 records
for spec in "0 65536" "65535 65536" "65536 65536" "200000 65536" "300000 65536"; do
  set -- $spec
  /tmp/zlib_body_emit "$1" "$2" "/tmp/ztb_$1.body" "/tmp/ztb_$1.raw"
done
python3 - <<'PEOF' || rc=1
import zlib, struct
ok = True
for n, grain in ((0,65536),(65535,65536),(65536,65536),(200000,65536),(300000,65536)):
    body = open(f"/tmp/ztb_{n}.body","rb").read()
    raw  = open(f"/tmp/ztb_{n}.raw","rb").read()
    try:
        pos, recs, arch, ended = 0, 0, b"", False
        while pos + 4 <= len(body):
            hdr = struct.unpack(">I", body[pos:pos+4])[0]; pos += 4
            if hdr == 0: ended = True; break
            stored = bool(hdr & 0x80000000); ln = hdr & 0x7fffffff
            payload = body[pos:pos+ln]; pos += ln; recs += 1
            arch += payload if stored else zlib.decompress(payload)
        exp = max(1, (n + grain - 1)//grain)
        good = ended and pos == len(body) and arch == raw and recs == exp
    except Exception as e:
        good, recs = False, e
    print(f"  [{'PASS' if good else 'FAIL'}] multi-record body round-trip"
          f"{'':<27} n={n} records={recs}")
    ok = ok and good
print(f"\nmulti-record cross-check: {'all sizes OK' if ok else 'FAILURES PRESENT'}")
raise SystemExit(0 if ok else 1)
PEOF

exit $rc
