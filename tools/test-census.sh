#!/usr/bin/env bash
# Host tests for the neighbour census (awdl_census.h), compiled unchanged from the
# library source. The mDNS fixtures are real bytes captured off the air, which is
# how we learned that AirDrop instance labels are opaque hex ids and cannot name a
# device -- the census looks at all mDNS precisely because of that.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
cc -O2 -Wall -Wextra -Wno-unused-function -o /tmp/census_test tools/census_test.c
exec /tmp/census_test
