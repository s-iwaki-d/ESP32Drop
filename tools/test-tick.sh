#!/usr/bin/env bash
# Host tests for the window-anchored transmit schedule (awdl_tick.h), compiled unchanged
# from the firmware source.
#
# Case 1 is the one that matters: 36 cells of wake jitter x per-transmit blocking, against
# three real mask shapes, asserting the window's full instant count is delivered in every
# one. This project has twice shipped a cadence whose output nobody computed.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
cc -O2 -Wall -Wextra -Wno-unused-function -o /tmp/tick_test tools/tick_test.c -lm
exec /tmp/tick_test
