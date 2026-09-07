#!/usr/bin/env bash
# The two library rules the COMPILER cannot enforce, enforced here instead.
#
# Both were measured to be unenforced by the toolchain:
#   - Nothing stops library code calling Serial. It links fine; it is simply wrong for a
#     library to decide that a user's port is its log, and every one of these lines is
#     produced on a task where a 250 ms USB CDC stall is a lost availability window.
#   - Nothing stops src/airdrop reaching into src/awdl. Measured with a probe library:
#     "../../awdl/core/foo.h" resolves perfectly well. The layer split is a discipline,
#     and a discipline with no check is a wish.
#
# Comments are STRIPPED before the Serial check rather than grepped around, because the
# reasoning about why Serial is absent is written in those comments and mentions it by
# name -- a pattern-matched gate would fire on its own rationale.
#
# Run tools/check-library.sh --self-test to see both rules fail on purpose.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SRC="$ROOT/ESP32Drop/src"
rc=0

strip_comments() { # $1 = file. Removes /* */ and // , keeps line numbers.
  perl -0777 -pe 's{/\*.*?\*/}{ my $m=$&; $m =~ s/[^\n]//g; $m }ges; s{//[^\n]*}{}g' "$1"
}

echo "== rule 1: no Serial in library source =="
while IFS= read -r f; do
  hits=$(strip_comments "$f" | grep -nE '\bSerial\b' || true)
  if [[ -n "$hits" ]]; then
    echo "$hits" | sed "s|^|${f#$ROOT/}:|"
    rc=1
  fi
done < <(find "$SRC" -name '*.cpp' -o -name '*.h')
[[ $rc -eq 0 ]] && echo "  clean"

# Both direction rules look at the INCLUDE TARGET, never at the whole grep line. The first
# version of this matched `grep -i airdrop` against the line, and the line contains the
# path -- which starts with ESP32Drop/ for every file in the library. It reported all
# eighty includes as violations. A checker that cannot tell its own repository name from a
# layer boundary is not a checker.
targets() {   # $1 = directory. prints "relpath<TAB>include-target" for every #include "..."
  grep -rn '#include[[:space:]]*"' "$1" 2>/dev/null \
    | sed -E "s|^$ROOT/||; s|^([^:]+):[0-9]+:[[:space:]]*#include[[:space:]]*\"([^\"]+)\".*|\1\t\2|"
}

echo "== rule 2: src/airdrop includes nothing from src/awdl but ESP32AWDL.h =="
bad=$(targets "$SRC/airdrop" | awk -F'\t' '$2 ~ /awdl|AWDL/ && $2 !~ /ESP32AWDL\.h$/')
if [[ -n "$bad" ]]; then echo "$bad"; rc=1; else echo "  clean"; fi

echo "== rule 3: src/awdl includes nothing from src/airdrop =="
# The rule runs BOTH ways, and there is no exception any more. There was exactly one, for
# one commit: the mDNS responder had to run inside the AWDL receive path, so it lived in
# the link layer and reached across for the codec. The RX tap removed it -- the responder
# is registered from src/airdrop now, and the link layer calls it without knowing what it
# is. If this rule ever needs an exception again, add it here BY NAME with what removes
# it, rather than widening the pattern.
bad3=$(targets "$SRC/awdl" | awk -F'\t' '$2 ~ /airdrop|ad_/')
if [[ -n "$bad3" ]]; then echo "$bad3"; rc=1; else echo "  clean"; fi

if [[ "${1:-}" == "--self-test" ]]; then
  echo "== self-test: both rules must FAIL when broken =="
  t="$SRC/awdl/core/__gatetest.h"; printf '#pragma once\nstatic void _g(){ Serial.print("x"); }\n' > "$t"
  strip_comments "$t" | grep -qE '\bSerial\b' && echo "  rule 1 fires: OK" || { echo "  rule 1 DID NOT FIRE"; rm -f "$t"; exit 2; }
  rm -f "$t"
  t2="$SRC/airdrop/core/__gatetest.h"; printf '#pragma once\n#include "../../awdl/core/awdl_ring.h"\n' > "$t2"
  grep -rn '#include' "$SRC/airdrop" | grep -i awdl | grep -qv 'ESP32AWDL\.h' && echo "  rule 2 fires: OK" || { echo "  rule 2 DID NOT FIRE"; rm -f "$t2"; exit 2; }
  rm -f "$t2"
  t3="$SRC/awdl/core/__gatetest.h"; printf '#pragma once\n#include "../../airdrop/core/ad_http.h"\n' > "$t3"
  targets "$SRC/awdl" | awk -F'\t' '$2 ~ /airdrop|ad_/' | grep -q . \
    && echo "  rule 3 fires: OK" || { echo "  rule 3 DID NOT FIRE"; rm -f "$t3"; exit 2; }
  rm -f "$t3"
fi
exit $rc
