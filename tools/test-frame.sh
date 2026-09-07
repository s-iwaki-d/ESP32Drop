#!/usr/bin/env bash
# Host tests for the AWDL MIF builder (awdl_frame.h), compiled unchanged from the
# firmware source.
#
# The golden vectors in testdata/mif_golden.inc were produced by the PRE-EXTRACTION
# build_mif -- pulled verbatim out of git and compiled on the host with a deterministic
# clock -- so requiring an exact match is what makes "the extraction changed no bytes"
# a measured fact instead of a claim. The structural checks are there because golden
# vectors would freeze a wrong frame just as happily as a right one.
#
# The MIF's TLV lengths are hand-written literals and a wrong one shifts every
# following TLV silently, so also check a live MIFHEX capture after changing
# anything here.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
cc -O2 -Wall -Wextra -Wno-unused-function -o /tmp/frame_test tools/frame_test.c
exec /tmp/frame_test
