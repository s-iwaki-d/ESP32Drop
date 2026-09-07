#!/usr/bin/env bash
# Host tests for ad_cpio.h, compiled unchanged from the firmware source.
#
# The archive comes from a remote device, so most of these cases are malformed on purpose.
# The fixture the good cases use is not invented: it is the exact two-entry shape every one
# of the thirteen archives in the capture corpus had -- a zero-byte "." directory followed
# by the file. That shape is why the iterator parses mode at all.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
cc -O2 -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -o /tmp/cpio_test tools/cpio_test.c
exec /tmp/cpio_test
