#!/usr/bin/env bash
# Host tests for ad_dnssd.h's RFC 6762 7.1 known-answer decision, compiled unchanged from
# the firmware source.
#
# This is the function that decides when NOT to answer an mDNS query, so most of these
# cases assert that it REFUSES to suppress. Every way it can be wrong ends the same way:
# the badge goes quiet and stops appearing in the AirDrop UI, which this project has
# twice needed a human to notice.
#
# Fixtures are real queries captured off the air, plus
# synthetic messages for cases the air has not produced -- an expiring TTL, a pointer
# loop, a forward pointer, every truncation of a message that would otherwise suppress.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
cc -O2 -Wall -Wextra -Wno-unused-function -o /tmp/dnssd_test tools/dnssd_test.c
exec /tmp/dnssd_test
