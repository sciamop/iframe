#!/bin/bash
# Regenerates every icon from scripts/make-icon.py: iPad app icon set (light/dark/tinted)
# and the macOS iFrame.icns for iFrame Host. Needs rsvg-convert and ImageMagick (brew).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

python3 scripts/make-icon.py
rsvg-convert -w 1024 -h 1024 Design/icon.svg -o Design/icon-1024.png

SET=Client/Assets.xcassets/AppIcon.appiconset
cp Design/icon-1024.png "$SET/icon-1024.png"
rsvg-convert -w 1024 -h 1024 Design/icon-transparent.svg -o "$SET/icon-dark-1024.png"
magick "$SET/icon-dark-1024.png" -colorspace Gray -level 0%,80% "$SET/icon-tinted-1024.png"

# In-app logo: the glyph alone, transparent, cropped tight.
MARK=Client/Assets.xcassets/IFrameMark.imageset; mkdir -p "$MARK"
rsvg-convert -w 1536 -h 1536 Design/icon-transparent.svg -o "$TMP/mark.png"
magick "$TMP/mark.png" -trim +repage "$MARK/iframe-mark.png"
cat > "$MARK/Contents.json" <<'JSON'
{ "images" : [ { "filename" : "iframe-mark.png", "idiom" : "universal" } ],
  "info" : { "author" : "xcode", "version" : 1 } }
JSON

# Accent color used for tints across the app.
ACCENT_HEX=$(python3 -c "import re;print(re.search(r'ACCENT = \"#([0-9A-Fa-f]{6})', open('scripts/make-icon.py').read()).group(1))")
COLOR=Client/Assets.xcassets/AccentColor.colorset; mkdir -p "$COLOR"
R=$((16#${ACCENT_HEX:0:2})); G=$((16#${ACCENT_HEX:2:2})); B=$((16#${ACCENT_HEX:4:2}))
cat > "$COLOR/Contents.json" <<JSON
{ "colors" : [ { "idiom" : "universal", "color" : { "color-space" : "srgb",
    "components" : { "red" : "$(printf '0x%02X' $R)", "green" : "$(printf '0x%02X' $G)", "blue" : "$(printf '0x%02X' $B)", "alpha" : "1.000" } } } ],
  "info" : { "author" : "xcode", "version" : 1 } }
JSON

# macOS: 824px rounded tile centered on a 1024 canvas (Apple's icon grid).
magick Design/icon-1024.png -resize 824x824 "$TMP/tile.png"
magick -size 824x824 xc:black -fill white -draw "roundrectangle 0,0,823,823,185,185" "$TMP/mask.png"
magick "$TMP/tile.png" "$TMP/mask.png" -alpha off -compose CopyOpacity -composite "$TMP/rounded.png"
magick -size 1024x1024 xc:none "$TMP/rounded.png" -gravity center -compose over -composite Design/icon-mac-1024.png
mkdir -p "$TMP/iFrame.iconset"
for sz in 16 32 128 256 512; do
  magick Design/icon-mac-1024.png -resize ${sz}x${sz} "$TMP/iFrame.iconset/icon_${sz}x${sz}.png"
  magick Design/icon-mac-1024.png -resize $((sz*2))x$((sz*2)) "$TMP/iFrame.iconset/icon_${sz}x${sz}@2x.png"
done
iconutil -c icns "$TMP/iFrame.iconset" -o Design/iFrame.icns
echo "icons rebuilt"
