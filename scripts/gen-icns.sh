#!/bin/bash
# Generate a macOS .icns from an SVG using built-in sips + iconutil.
# Usage: gen-icns.sh <input.svg> <output.icns>
set -euo pipefail

SVG=$1
OUT=$2
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

ICONSET="$TMP/icon.iconset"
mkdir -p "$ICONSET"

render() {
    local size=$1 name=$2
    sips -s format png "$SVG" --out "$ICONSET/$name" -Z "$size" > /dev/null 2>&1
}

render 16   icon_16x16.png
render 32   icon_16x16@2x.png
render 32   icon_32x32.png
render 64   icon_32x32@2x.png
render 128  icon_128x128.png
render 256  icon_128x128@2x.png
render 256  icon_256x256.png
render 512  icon_256x256@2x.png
render 512  icon_512x512.png
render 1024 icon_512x512@2x.png

iconutil -c icns "$ICONSET" -o "$OUT"
