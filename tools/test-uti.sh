#!/usr/bin/env bash
# core/ad_uti.h against Apple's own MIME->UTI answers, plus the resolve rules.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
cc -O2 -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined \
   -o /tmp/uti_test tools/uti_test.c
exec /tmp/uti_test
