#!/bin/bash
# Build V3 + V4 firmware with arduino-cli (Arduino ESP32 core 3.0.x + ThingPulse SSD1306 lib)
# Output: dist/esp32dj-v3.bin, dist/esp32dj-v4.bin (merged, flash at 0x0) and *-ota.bin (app only)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
python3 "$ROOT/tools/embed_webui.py"
SK="$ROOT/firmware/ESP32DJ"
mkdir -p "$ROOT/dist"
EXTRA=()
[ -n "$CTAGS_PATH" ] && EXTRA+=(--build-property "runtime.tools.ctags.path=$CTAGS_PATH")
build() { # name fqbn-options define
  local name=$1 opts=$2 def=$3
  arduino-cli compile --fqbn "espressif:esp32:esp32s3:$opts" --build-path "/tmp/build-$name" \
    --build-property "compiler.cpp.extra_flags=-MMD -c -D$def" \
    --build-property "compiler.c.extra_flags=-MMD -c -D$def" "${EXTRA[@]}" "$SK" 2>&1 \
    | grep -vE "^Error initializing instance" | grep -E "error|Sketch uses|Global variables" || true
  local B="/tmp/build-$name" F="$ROOT/docs/firmware/$name"
  test -f "$B/ESP32DJ.ino.bin"
  mkdir -p "$F"
  cp "$B/ESP32DJ.ino.bootloader.bin" "$F/bootloader.bin"
  cp "$B/ESP32DJ.ino.partitions.bin" "$F/partitions.bin"
  cp "$(arduino-cli config get directories.user)/hardware/espressif/esp32/tools/partitions/boot_app0.bin" "$F/boot_app0.bin" 2>/dev/null \
    || cp "$HOME/Arduino/hardware/espressif/esp32/tools/partitions/boot_app0.bin" "$F/boot_app0.bin"
  cp "$B/ESP32DJ.ino.bin" "$F/esp32dj.bin"
  cp "$B/ESP32DJ.ino.bin" "$ROOT/dist/esp32dj-$name-ota.bin"
}
build v3 "FlashSize=8M,PartitionScheme=min_spiffs,PSRAM=disabled,CPUFreq=240,CDCOnBoot=default" DJ_BOARD_V3
build v4 "FlashSize=16M,PartitionScheme=min_spiffs,PSRAM=enabled,CPUFreq=240,CDCOnBoot=cdc" DJ_BOARD_V4
ls -la "$ROOT/dist" "$ROOT/docs/firmware/"*
