#!/usr/bin/env bash
# Compile every example with the warnings the normal build suppresses, and fail on the ones
# that are defects. Own build dirs, so it never disturbs .build-example.
#
# WHY THIS EXISTS. tools/build-examples.sh passes --warnings none, which is reasonable for a
# tree that compiles the whole ESP32 core -- and it is how a shadowed local declaration once
# built clean, published a window nobody had filled in, and left the transmit gate closed
# for the entire life of the boot while every host suite passed. The host suites cannot
# cover this class: it lives in the sketch and in the port .cpp files, which they do not
# compile. Only -Wshadow saw it.
#
# The examples between them compile BOTH builds of the library: PrintReceivedFiles and
# ShowReceivedImage the receiver, GreetingCard (ESP32DROP_SENDER in its build_opt.h) the
# sender. A warning in either half is caught here.
#
# Two categories are deliberately NOT fatal, and it matters that this is a decision rather
# than an oversight:
#   -Wunused-function  the house style puts static functions in dependency-free headers so
#                      the same code compiles into the firmware and into a host test. Not
#                      every translation unit uses every one. That is the design.
#   -Wvolatile         C++20 deprecates compound assignment on volatile. The counters this
#                      fires on are cross-task and volatile on purpose.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$ESP32DROP_ROOT"

# GCC tags these '[-Wformat=]', not '[-Wformat]'; the pattern must accept the suffix or the
# whole format class becomes invisible.
FATAL_RE='\[-W(shadow|uninitialized|maybe-uninitialized|return-type|unused-variable|unused-but-set-variable|sign-compare|parentheses|sequence-point|array-bounds|restrict|nonnull|format)[=a-z-]*\]'

rc=0
for d in ESP32Drop/examples/*/; do
  name=$(basename "$d")
  # The log must NOT live inside --build-path: arduino-cli cleans that directory.
  LOG="$ESP32DROP_ROOT/.build-lint-$name.log"
  # ALWAYS a clean build: cached object files emit no warnings, so an incremental lint over
  # an unchanged tree would "pass" without compiling anything.
  rm -rf "$ESP32DROP_ROOT/.build-lint/$name"
  printf '%-24s ' "$name"
  "$ACLI" --config-file "$CLI_CONFIG" compile \
    --fqbn "$FQBN" "${ACLI_LIB[@]}" --warnings all \
    --build-property "compiler.c.elf.extra_flags=$ELF_EXTRA_FLAGS" \
    --build-property "compiler.cpp.extra_flags=-Wshadow" \
    --build-path "$ESP32DROP_ROOT/.build-lint/$name" \
    "$d" > "$LOG" 2>&1 || { tail -30 "$LOG"; echo "COMPILE FAILED"; exit 1; }

  # A lint that did not look at anything must not say "clean".
  [[ -s "$LOG" ]] || { echo "FAIL: no compiler output captured -- the lint did not run"; exit 2; }
  # This tree has hundreds of warnings from the core, so zero means --warnings is not in effect.
  grep -q ": warning:" "$LOG" || { echo "FAIL: zero warnings anywhere -- --warnings is not in effect"; exit 2; }
  # The gate must be shown to REACH the library, not merely to have been written.
  grep -qE "^$ESP32DROP_ROOT/ESP32Drop/src/.*: warning:" "$LOG" \
    || { echo "FAIL: no warning from ESP32Drop/src -- the gate does not reach the library"; exit 2; }

  # Only our own files: the core's warnings are not ours to fix and would drown these.
  HITS=$(grep -E "^$ESP32DROP_ROOT/ESP32Drop/(examples/$name/[A-Za-z0-9_]+\.(ino|h)|src/.*\.(h|cpp)):[0-9]+:[0-9]+: warning:" "$LOG" \
         | grep -E "$FATAL_RE" || true)
  if [[ -n "$HITS" ]]; then
    echo "FAIL"; echo "$HITS"; rc=1
  else
    echo "clean"
  fi
done
exit $rc
