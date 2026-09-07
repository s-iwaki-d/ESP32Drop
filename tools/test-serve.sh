#!/usr/bin/env bash
# Host tests for the AirDrop HTTP DISPATCH layer (awdl_serve.h), compiled unchanged
# from the firmware source. Where test-http.sh covers the byte reader and de-chunker,
# this covers everything between "TLS handshake done" and "a body handler runs":
# request-line validation, the DESYNC detector, header parsing, the response-then-
# drain ordering for /Discover and /Ask, the /Upload routing, and the idle/incomplete
# closes. The transport, the clock and the firmware side effects are behind callbacks
# so the SAME dispatch code runs here as on the device.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
cc -O2 -Wall -Wextra -Wno-unused-function -o /tmp/serve_test tools/serve_test.c
exec /tmp/serve_test
