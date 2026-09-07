#!/usr/bin/env bash
# Compile every example under ESP32Drop/examples/.
#
# WHY IT IS A SCRIPT AND NOT A ONE-OFF. An example is the only part of a library that
# proves the public API is usable, and it is the first thing that rots: it is not in the
# build the developer runs, so a signature change compiles the firmware and breaks the
# example silently. This is in the same class as the host suites -- run it before pushing.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$ESP32DROP_ROOT"
rc=0
for d in ESP32Drop/examples/*/; do
  name=$(basename "$d")
  printf '%-24s ' "$name"
  if out=$("$ACLI" --config-file "$CLI_CONFIG" compile \
        --fqbn "$FQBN" "${ACLI_LIB[@]}" --warnings none \
        --build-property "compiler.c.elf.extra_flags=$ELF_EXTRA_FLAGS" \
        --build-path "$ESP32DROP_ROOT/.build-example" "$d" 2>&1); then
    echo "OK   $(echo "$out" | grep -oE '[0-9]+ bytes \([0-9]+%\) of program storage' | head -1)"
  else
    echo "FAILED"; echo "$out" | grep -E 'error:' | head -5; rc=1
  fi
done
exit $rc
