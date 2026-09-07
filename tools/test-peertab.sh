#!/bin/sh
# Host test for ESP32Drop/src/awdl/core/awdl_peertab.h.
# The header is compiled UNCHANGED, which is the point: the eviction and the RSSI
# averaging are checked without a device, on the same source the firmware links.
set -e
cd "$(dirname "$0")/.."
cc -O2 -Wall -Wextra -I ESP32Drop/src/awdl/core -o /tmp/peertab_test tools/peertab_test.c
exec /tmp/peertab_test
