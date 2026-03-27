#!/bin/bash
#
# build-ipad.sh -- Build FWDisplay iPad app (.deb package)
#
# Prerequisites:
#   Theos installed at ~/theos (https://theos.dev)
#   iOS 9.x SDK
#
# Output: dist/FWDisplay_<version>.deb
#

set -e
cd "$(dirname "$0")/ipad"

echo "[build] Building FWDisplay iPad app..."
make clean 2>/dev/null || true
make package

# Find the latest .deb
DEB=$(ls -t packages/*.deb 2>/dev/null | head -1)

if [ -z "$DEB" ]; then
    echo "[build] ERROR: No .deb package produced"
    exit 1
fi

mkdir -p ../dist
cp "$DEB" ../dist/
BASENAME=$(basename "$DEB")

make clean 2>/dev/null || true

echo "[build] Done -> dist/$BASENAME"
ls -lh "../dist/$BASENAME"
