#!/usr/bin/env bash
# Host tests for the SPSC ring cursor (awdl_ring.h), compiled unchanged from the firmware
# source. The frame ring's producer runs in the WiFi callback and must never block; the
# dump ring's consumer holds a UART measured stalling for 250 ms. Both failure modes here
# -- full mistaken for empty, empty mistaken for full -- are silent on hardware.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
cc -O2 -Wall -Wextra -Wno-unused-function -o /tmp/ring_test tools/ring_test.c
exec /tmp/ring_test
