# Shared settings for the ESP32Drop build scripts: build-examples.sh, flash-example.sh, lint.sh.
# shellcheck shell=bash
set -euo pipefail

ESP32DROP_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export ESP32DROP_ROOT

# arduino-cli: $ACLI if set, else one on PATH, else the copy bundled in the Arduino IDE.
if [[ -z "${ACLI:-}" ]]; then
  if command -v arduino-cli >/dev/null 2>&1; then
    ACLI="$(command -v arduino-cli)"
  else
    ACLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
  fi
fi

ARDUINO_USER_DIR="${ARDUINO_USER_DIR:-$HOME/Documents/Arduino}"

CLI_CONFIG="$ESP32DROP_ROOT/.build/arduino-cli.yaml"
mkdir -p "$ESP32DROP_ROOT/.build"
cat > "$CLI_CONFIG" <<YAML
directories:
  data: $HOME/Library/Arduino15
  user: $ARDUINO_USER_DIR
  downloads: $HOME/Library/Arduino15/staging
board_manager:
  additional_urls:
    - https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
build_cache:
  path: $ESP32DROP_ROOT/.build-cache
YAML

# The board the library was written and measured on: an M5Stack StopWatch (ESP32-S3R8) on
# the m5stack:esp32 core, version 3.3.8. PSRAM=opi is mandatory on that part (octal PSRAM)
# or the board dies before setup(); huge_app leaves room for mbedTLS; the USB options keep
# Serial and 1200 bps-touch upload working. Set FQBN in the environment for another
# ESP32-S3 board with PSRAM.
FQBN="${FQBN:-m5stack:esp32:m5stack_stopwatch:PartitionScheme=huge_app,PSRAM=opi,DebugLevel=error,USBMode=default,CDCOnBoot=cdc,UploadMode=cdc}"

# No linker flags are needed. Kept as an override point for experiments.
ELF_EXTRA_FLAGS="${ELF_EXTRA_FLAGS-}"

# ESP32Drop is compiled as a real Arduino LIBRARY, exactly as a user's IDE would compile it:
# --library puts ESP32Drop/src on the include path and compiles src/**/*.cpp. Library
# discovery is include-driven, so a sketch must #include <ESP32AWDL.h> by bare name.
ACLI_LIB=(--library "$ESP32DROP_ROOT/ESP32Drop")

detect_port() {
  if [[ -n "${PORT:-}" ]]; then echo "$PORT"; return; fi
  "$ACLI" --config-file "$CLI_CONFIG" board list --json \
    | python3 -c '
import json, sys
for p in json.load(sys.stdin).get("detected_ports", []):
    port = p.get("port", {})
    props = port.get("properties", {})
    if props.get("vid", "").lower() == "0x303a":
        print(port["address"]); break
else:
    sys.exit("no ESP32-S3 board found (VID 0x303A); set PORT=/dev/cu.usbmodemXXX")
'
}
