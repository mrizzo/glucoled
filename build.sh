#!/bin/bash
# Compile (and optionally flash) the sketch.
#   ./build.sh            compile only
#   ./build.sh upload     compile + flash to the first USB serial port found
# Settings are entered on the board's setup page; secrets.h (optional) only sets defaults.
#   NOSECRETS=1           build without secrets.h (for the public flasher; see make_site.sh)
# Pick the board with BOARD= (default ideaspark):
#   BOARD=ideaspark   ideaspark ESP32 1.9" ST7789 (CH340 USB)
#   BOARD=tdisplays3  LilyGO T-Display S3 (ESP32-S3 native USB)
set -euo pipefail
cd "$(dirname "$0")"
BOARD="${BOARD:-ideaspark}"
case "$BOARD" in
  ideaspark)  HDR=board_ideaspark_19.h; FQBN="esp32:esp32:esp32:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB" ;;
  tdisplays3) HDR=board_tdisplay_s3.h;  FQBN="esp32:esp32:lilygo_t_display_s3:CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB" ;;
  *) echo "unknown BOARD=$BOARD (ideaspark | tdisplays3)"; exit 1 ;;
esac
FLAGS="-include $(pwd)/$HDR"
# Stamp the git commit into the firmware (shown on the debug and setup pages); "+" = uncommitted changes.
GIT=$(git rev-parse --short HEAD 2>/dev/null || true)
if [ -n "$GIT" ]; then
  git diff --quiet HEAD -- . 2>/dev/null || GIT="$GIT+"
  FLAGS="$FLAGS -DFW_GIT=$GIT"
fi
BUILD="build/$BOARD"
if [ "${NOSECRETS:-}" = 1 ]; then          # public firmware: ignore secrets.h even if present
  FLAGS="$FLAGS -DGLUCOLED_NO_SECRETS"; BUILD="build/public-$BOARD"
fi
arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD" \
  --build-property "compiler.cpp.extra_flags=$FLAGS" \
  --build-property "compiler.c.extra_flags=$FLAGS" .
if [ "${1:-}" = "upload" ]; then
  PORT="${PORT:-$(ls /dev/cu.usbserial-* /dev/cu.wchusbserial* /dev/cu.usbmodem* 2>/dev/null | head -1 || true)}"
  [ -n "$PORT" ] || { echo "board not found: plug it in with a USB-C data cable"; exit 1; }
  echo "flashing $PORT"
  arduino-cli upload --fqbn "$FQBN" --input-dir "$BUILD" -p "$PORT" .
  echo "serial log: arduino-cli monitor -p $PORT -c baudrate=115200"
fi
