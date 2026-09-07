#!/usr/bin/env bash
# Host tests for ad_bplist.h against goldens from Python plistlib.
# Regenerate the goldens with tools/gen_bplist_golden.sh.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
cc -O2 -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -o /tmp/bplist_test tools/bplist_test.c
exec /tmp/bplist_test
