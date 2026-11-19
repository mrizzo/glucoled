#!/bin/bash
# Build the browser flasher site into site/: public firmware for both boards (never with
# secrets.h), an ESP Web Tools manifest, and the install page from web/.
#   ./make_site.sh            build firmware + site
#   ./make_site.sh serve      ...then serve it on http://localhost:8000 (Web Serial works on localhost)
# GitHub Actions runs this on every push to main and publishes site/ to GitHub Pages.
set -euo pipefail
cd "$(dirname "$0")"
OUT=site
VER=$(sed -n 's/.*FW_VERSION *= *"\([^"]*\)".*/\1/p' glucoled.ino)
BOOT_APP0=$(ls ~/.arduino15/packages/esp32/hardware/esp32/3.2.0/tools/partitions/boot_app0.bin \
               ~/Library/Arduino15/packages/esp32/hardware/esp32/3.2.0/tools/partitions/boot_app0.bin 2>/dev/null | head -1 || true)
[ -n "$BOOT_APP0" ] || { echo "boot_app0.bin not found: install esp32:esp32@3.2.0"; exit 1; }

NOSECRETS=1 BOARD=ideaspark  ./build.sh
NOSECRETS=1 BOARD=tdisplays3 ./build.sh

rm -rf "$OUT"; mkdir -p "$OUT/fw"
# chip folder <- board build. ESP Web Tools picks the build matching the chip it detects.
for pair in "esp32 ideaspark" "esp32s3 tdisplays3"; do
  set -- $pair
  mkdir -p "$OUT/fw/$1"
  for f in bootloader partitions; do cp "build/public-$2/glucoled.ino.$f.bin" "$OUT/fw/$1/$f.bin"; done
  cp "build/public-$2/glucoled.ino.bin" "$OUT/fw/$1/glucoled.bin"
  # Belt and braces: if there's a local secrets.h, none of its values may be in a public binary.
  # (Prints only a count, never the values.)
  if [ -f secrets.h ]; then
    hits=$(grep -o '"[^"]\{4,\}"' secrets.h | tr -d '"' | grep -v '^YOUR_' | sort -u |
           while IFS= read -r v; do
             if grep -qF -- "$v" glucoled.ino setup_page.ino; then continue; fi   # public anyway (e.g. "JST-9")
             if grep -qaF -- "$v" "$OUT/fw/$1/glucoled.bin"; then echo x; fi
           done | wc -l | tr -d ' ')
    [ "$hits" = 0 ] || { echo "STOP: $hits value(s) from secrets.h found in the $1 build"; exit 1; }
  fi
done
cp "$BOOT_APP0" "$OUT/fw/boot_app0.bin"
cp -R web/. "$OUT/"

parts() {   # bootloader offset differs: 0x1000 on ESP32, 0x0 on ESP32-S3
  echo "[{\"path\":\"fw/$1/bootloader.bin\",\"offset\":$2},{\"path\":\"fw/$1/partitions.bin\",\"offset\":32768},"\
       "{\"path\":\"fw/boot_app0.bin\",\"offset\":57344},{\"path\":\"fw/$1/glucoled.bin\",\"offset\":65536}]"
}
cat > "$OUT/manifest.json" <<EOF
{
  "name": "Glucoled",
  "version": "$VER",
  "new_install_prompt_erase": true,
  "builds": [
    {"chipFamily": "ESP32",    "parts": $(parts esp32 4096)},
    {"chipFamily": "ESP32-S3", "parts": $(parts esp32s3 0)}
  ]
}
EOF
echo "site/ ready: Glucoled $VER"
if [ "${1:-}" = serve ]; then
  echo "open http://localhost:8000 in Chrome or Edge"
  cd "$OUT" && python3 -m http.server 8000
fi
