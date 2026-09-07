#!/usr/bin/env bash
# Regression test for the AWDL gauge. Replays a recorded PSF trace through the REAL
# firmware code (awdl_gauge.h) on the host and fails if any phase sample it would
# feed the window PLL is a whole AW or more away from the truth.
#
# This is the test that was missing. The defect it guards against shipped, ran for
# hours, aimed 4125 transmissions into an empty window, and was found only because a
# human noticed the badge had disappeared from the AirDrop UI.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
# testdata/ is deliberately outside .build, which the toolchain wipes.
TRACE="${1:-testdata/trace3.txt}"
[[ -f "$TRACE" ]] || { echo "no trace at $TRACE -- capture one: save the PS lines from a SYNC_TRACE build"; exit 2; }

echo "### baseline: the estimator that shipped (per-sender B, latched)"
python3 tools/replay_b.py "$TRACE" | tail -8

echo
echo "### under test: awdl_gauge.h, compiled for the host"
cc -O2 -o /tmp/gauge_test tools/gauge_test.c
/tmp/gauge_test "$TRACE" > /tmp/gauge_out.txt
python3 tools/score_gauge.py /tmp/gauge_out.txt "$TRACE"
