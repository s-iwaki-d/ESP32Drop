#!/usr/bin/env bash
# Host tests for the master election (awdl_elect.h), compiled unchanged from the
# firmware source. The two worst failures this project has had were both election
# bugs that reached the device and were noticed only because the badge vanished from
# AirDrop; these fixtures exist so the next change to the rules can be argued about
# before it is flashed.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
cc -O2 -Wall -Wextra -Wno-unused-function -o /tmp/elect_test tools/elect_test.c
exec /tmp/elect_test
