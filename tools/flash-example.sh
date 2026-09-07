#!/usr/bin/env bash
# Build and flash one of the library's examples.
#
#   ./tools/flash-example.sh GreetingCard
#
# The examples are what a user actually runs, and an example nobody flashes is an example
# nobody has seen work -- GreetingCard once compiled and linked in a configuration that
# could not wake a peer. Each example gets its own build dir.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$ESP32DROP_ROOT"
NAME="${1:-}"
[[ -z "$NAME" ]] && { echo "usage: $0 <ExampleName>"; ls ESP32Drop/examples; exit 2; }
SKETCH="$ESP32DROP_ROOT/ESP32Drop/examples/$NAME"
[[ -d "$SKETCH" ]] || { echo "no such example: $NAME"; ls ESP32Drop/examples; exit 2; }
BUILD="$ESP32DROP_ROOT/.build-ex-$NAME"

# build_opt.h beside a sketch is how a define reaches the LIBRARY as well as the sketch;
# say so out loud, because a missing one is silent and produces a build that links and
# cannot work.
if [[ -f "$SKETCH/build_opt.h" ]]; then
  echo "==> $NAME build_opt.h: $(tr '\n' ' ' < "$SKETCH/build_opt.h")"
fi

PORT_ADDR="$(detect_port)"
echo "==> building $NAME"
"$ACLI" --config-file "$CLI_CONFIG" compile \
  --fqbn "$FQBN" "${ACLI_LIB[@]}" --warnings none \
  --build-property "compiler.c.elf.extra_flags=$ELF_EXTRA_FLAGS" \
  --build-path "$BUILD" "$SKETCH"
echo "==> flashing $NAME to $PORT_ADDR"
exec "$ACLI" --config-file "$CLI_CONFIG" upload \
  --fqbn "$FQBN" --port "$PORT_ADDR" --input-dir "$BUILD" "$SKETCH"
