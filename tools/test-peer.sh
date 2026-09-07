#!/usr/bin/env bash
# Host tests for the mesh window PLL (awdl_peer.h), compiled unchanged from the firmware
# source.
#
# The central case is DIFFERENTIAL: a verbatim transcription of the loop as it was written
# inline in handle_sync(), fed the same samples at the same timestamps, with every field
# of both states compared for exact bitwise equality after each one. Doubles with ==,
# deliberately -- the two do identical operations in identical order, so anything short of
# exact equality is a real difference.
#
# This loop decides where in the AW cycle the badge thinks it is, and the transmit gate
# takes that at face value. When it was last wrong the badge fired 4,125 frames into a
# slot nobody was listening on and nothing in the firmware noticed.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
cc -O2 -Wall -Wextra -Wno-unused-function -o /tmp/peer_test tools/peer_test.c -lm
exec /tmp/peer_test
