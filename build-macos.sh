#!/bin/bash
#
# build-macos.sh -- Build fwdisplay-sender for macOS (native)
#
# Prerequisites:
#   brew install jpeg-turbo
#

set -e
cd "$(dirname "$0")/sender"

NPROC=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

echo "[build] Building macOS (native)..."
make clean 2>/dev/null || true
make -j"$NPROC"

mkdir -p ../dist
cp fwdisplay-sender ../dist/fwdisplay-sender-macos
make clean 2>/dev/null || true

echo "[build] Done -> dist/fwdisplay-sender-macos"
ls -lh ../dist/fwdisplay-sender-macos
