#!/usr/bin/env bash
# Host tests for ad_pack.h, the cpio-odc writer the SEND path needs.
#
# TWO CHECKS, and the second is the one that matters. The C tests round-trip the archive
# through ad_cpio.h -- the reader a RECEIVER runs -- which proves our two halves agree.
# Then cpio(1) reads the same bytes: an implementation nobody here wrote, held to the same
# standard tools/test-bplist.sh holds itself to with Python's plistlib.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

cc -O2 -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined \
   -o "$OUT/pack_test" tools/pack_test.c
"$OUT/pack_test" "$OUT/archive.cpio"

echo "== cpio(1) reads it -- an implementation this project did not write =="
cd "$OUT"
# -i extracts, -t lists. 'odc' is what the writer emits and what Apple senders send.
if ! LIST=$(cpio -itv < archive.cpio 2>&1); then
  echo "  [FAIL] cpio(1) could not list the archive:"; echo "$LIST"; exit 1
fi
echo "$LIST" | sed 's/^/    /'
echo "$LIST" | grep -q 'card\.png' \
  && echo "  [PASS] cpio(1) lists ./card.png" \
  || { echo "  [FAIL] cpio(1) did not list ./card.png"; exit 1; }

mkdir -p x && cd x
cpio -i --quiet < ../archive.cpio 2>/dev/null || cpio -i < ../archive.cpio 2>/dev/null
if [ -f ./card.png ]; then
  SZ=$(wc -c < ./card.png | tr -d ' ')
  [ "$SZ" = 16 ] \
    && echo "  [PASS] cpio(1) extracted ./card.png, 16 bytes -- the payload we wrote" \
    || { echo "  [FAIL] extracted size $SZ, expected 16"; exit 1; }
  # The bytes, not just the length: a header that is right and a payload that is off by
  # one byte still extracts to the right size.
  printf '\211PNG\r\n\032\n\001\002\003\004\005\006\007\010' > want.bin
  cmp -s ./card.png want.bin \
    && echo "  [PASS] the extracted bytes are byte-exact" \
    || { echo "  [FAIL] extracted bytes differ"; exit 1; }
else
  echo "  [FAIL] cpio(1) did not extract ./card.png"; exit 1
fi
echo "  all cpio(1) checks passed"
