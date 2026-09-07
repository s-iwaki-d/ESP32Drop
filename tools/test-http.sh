#!/usr/bin/env bash
# Host tests for the AirDrop HTTP stream layer (awdl_http.h), compiled unchanged
# from the firmware source. The byte source is scripted in content AND TIME, because
# every defect this layer has produced was a timeout interacting with chunked
# framing and none is reachable without modelling when bytes arrive.
#
# It already earned its keep twice: it reproduces the field failure (a drain whose
# budget expires before the body starts arriving) and it found a latent bug in code
# that had already been flashed (a chunk appended inside the settle branch without
# re-judging completeness, so a DvZip end record could be consumed and ignored).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
cc -O2 -Wall -Wextra -Wno-unused-function -o /tmp/http_test tools/http_test.c
exec /tmp/http_test
