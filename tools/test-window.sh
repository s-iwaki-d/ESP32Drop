#!/usr/bin/env bash
# Host tests for the AWDL transmit gate (awdl_window.h), compiled unchanged from the
# firmware source.
#
# The central case is not an example: it is an EXHAUSTIVE comparison against a verbatim
# transcription of the in_ch6_window_now() this replaces, over every microsecond of the
# AW cycle and every mask shape that has been observed. This gate is the only thing
# between the badge and being invisible -- gate too tightly and the MIF never leaves,
# gate on the wrong slot and it leaves into an empty window, and both have happened.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
cc -O2 -Wall -Wextra -Wno-unused-function -o /tmp/window_test tools/window_test.c
exec /tmp/window_test
