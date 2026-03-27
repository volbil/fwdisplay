#!/bin/bash
#
# build-windows.sh -- Cross-compile fwdisplay-sender for Windows from macOS/Linux
#
# Prerequisites (macOS):
#   brew install mingw-w64 cmake nasm
#
# Prerequisites (Linux):
#   apt install mingw-w64 cmake nasm
#
# libturbojpeg is built from source automatically.
#

set -e
ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$ROOT_DIR/sender"

CC="x86_64-w64-mingw32-gcc"
WINDRES="x86_64-w64-mingw32-windres"

if ! command -v "$CC" &>/dev/null; then
    echo "[build] ERROR: Missing $CC"
    echo "[build] macOS:  brew install mingw-w64"
    echo "[build] Linux:  apt install mingw-w64"
    exit 1
fi

BUILD_DIR="$ROOT_DIR/build/win-x64"
VENDOR_DIR="$ROOT_DIR/vendor"
DIST_DIR="$ROOT_DIR/dist"

LIBJPEG_VERSION="3.1.0"
LIBJPEG_URL="https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/${LIBJPEG_VERSION}/libjpeg-turbo-${LIBJPEG_VERSION}.tar.gz"
LIBJPEG_PREFIX="$BUILD_DIR/libjpeg"

NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# --- Build libturbojpeg for Windows ---

if [ ! -f "$LIBJPEG_PREFIX/lib/libturbojpeg.a" ]; then
    echo "[build] Building libturbojpeg for Windows..."

    if [ ! -d "$VENDOR_DIR/libjpeg-turbo-${LIBJPEG_VERSION}" ]; then
        echo "[build] Downloading libjpeg-turbo ${LIBJPEG_VERSION}..."
        mkdir -p "$VENDOR_DIR"
        curl -sL "$LIBJPEG_URL" | tar xz -C "$VENDOR_DIR"
    fi

    rm -rf "$BUILD_DIR/libjpeg-build"
    mkdir -p "$BUILD_DIR/libjpeg-build"

    # Enable SIMD if NASM is available (2-4x faster JPEG encoding)
    SIMD_FLAGS="-DWITH_SIMD=OFF"
    if command -v nasm &>/dev/null; then
        echo "[build] NASM found -- enabling SIMD"
        SIMD_FLAGS="-DWITH_SIMD=ON -DCMAKE_ASM_NASM_COMPILER=$(which nasm)"
    else
        echo "[build] NASM not found -- SIMD disabled (install: brew install nasm)"
    fi

    cmake -S "$VENDOR_DIR/libjpeg-turbo-${LIBJPEG_VERSION}" \
          -B "$BUILD_DIR/libjpeg-build" \
          -DCMAKE_SYSTEM_NAME=Windows \
          -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
          -DCMAKE_C_COMPILER="$CC" \
          -DCMAKE_RC_COMPILER="$WINDRES" \
          -DCMAKE_INSTALL_PREFIX="$LIBJPEG_PREFIX" \
          -DENABLE_SHARED=OFF \
          -DENABLE_STATIC=ON \
          -DWITH_TURBOJPEG=ON \
          -DWITH_JPEG8=OFF \
          $SIMD_FLAGS \
          -DCMAKE_BUILD_TYPE=Release \
          > /dev/null 2>&1

    cmake --build "$BUILD_DIR/libjpeg-build" -j"$NPROC" > /dev/null 2>&1
    cmake --install "$BUILD_DIR/libjpeg-build" > /dev/null 2>&1

    echo "[build] libturbojpeg ready"
else
    echo "[build] libturbojpeg for Windows (cached)"
fi

# --- Compile sender ---

echo "[build] Compiling fwdisplay-sender.exe..."
mkdir -p "$BUILD_DIR/obj" "$DIST_DIR"

SRCS="sender.c usbmux.c capture_win.c"

OBJS=()
for src in $SRCS; do
    obj="$BUILD_DIR/obj/${src%.c}.o"
    $CC -O2 -Wall -Wextra \
        -I"$LIBJPEG_PREFIX/include" \
        -c "$SRC_DIR/$src" -o "$obj"
    OBJS+=("$obj")
done

$CC "${OBJS[@]}" -o "$DIST_DIR/fwdisplay-sender.exe" \
    -static \
    -L"$LIBJPEG_PREFIX/lib" -lturbojpeg \
    -lws2_32 -lgdi32

echo "[build] Done -> dist/fwdisplay-sender.exe"
ls -lh "$DIST_DIR/fwdisplay-sender.exe"
