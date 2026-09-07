#!/usr/bin/env bash
# Host tests for the per-sender residual table (awdl_src.h), compiled unchanged from the
# firmware source.
#
# Case 6 is a measurement, not an assertion: it compares the one-pass variance the ESRC
# line reports against a two-pass reference over the same samples, at the sender offsets
# this project has actually measured.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
cc -O2 -Wall -Wextra -Wno-unused-function -o /tmp/src_test tools/src_test.c -lm
exec /tmp/src_test
