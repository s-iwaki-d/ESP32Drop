#!/usr/bin/env bash
# Host tests for the public status API (awdl_status.h), compiled unchanged from the
# firmware source. The subject is the DERIVED answers -- "is it discoverable", "how is it
# going" -- because those are what a user acts on without checking the arithmetic, and
# because this firmware has twice reported perfect health while transmitting nothing.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
cc -O2 -Wall -Wextra -Wno-unused-function -o /tmp/status_test tools/status_test.c -lm
exec /tmp/status_test
