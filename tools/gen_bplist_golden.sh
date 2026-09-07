#!/usr/bin/env bash
# Regenerate testdata/bplist_golden.inc from Python's plistlib.
#
# The point is that plistlib is NOT our implementation. A golden the writer produced
# itself proves only that it is consistent with itself; these bytes come from the
# reference serialiser Apple's own tooling round-trips, so a byte-exact match is
# evidence about the FORMAT rather than about our reading of it.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
python3 "$ROOT/tools/gen_bplist_golden.py" > "$ROOT/testdata/bplist_golden.inc"
echo "wrote testdata/bplist_golden.inc ($(wc -l < "$ROOT/testdata/bplist_golden.inc") lines)"
