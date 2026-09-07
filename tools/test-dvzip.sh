#!/usr/bin/env bash
# Host tests for ad_dvzip.h, compiled unchanged from the firmware source.
#
# The framing lives in a core header precisely so it can be run here: it is driven entirely
# by bytes a remote Mac chooses, and the failure that produced these tests -- a stream
# declared cut and then returned as success -- is not visible from the device without a
# 71-second physical AirDrop.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
cc -O2 -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -o /tmp/dvzip_test tools/dvzip_test.c
exec /tmp/dvzip_test
