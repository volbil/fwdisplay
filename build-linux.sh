#!/bin/bash
#
# build-linux.sh -- Build fwdisplay-sender for Linux
#
# Native (on Linux):
#   apt install build-essential libturbojpeg0-dev libx11-dev libxext-dev
#   ./build-linux.sh
#
# Cross-compile (from macOS):
#   brew install FiloSottile/musl-cross/musl-cross cmake nasm
#   LINUX_SYSROOT=/path/to/sysroot ./build-linux.sh
#
#   Create a sysroot from a Debian/Ubuntu machine:
#     mkdir -p linux-sysroot/usr
#     scp -r linux:/usr/include linux-sysroot/usr/
#     scp -r linux:/usr/lib/x86_64-linux-gnu linux-sysroot/usr/lib/
#

set -e
ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$ROOT_DIR/sender"
DIST_DIR="$ROOT_DIR/dist"

mkdir -p "$DIST_DIR"

NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# --- Detect: native Linux or cross-compile ---

if [ "$(uname -s)" = "Linux" ]; then
    echo "[build] Building Linux (native)..."
    cd "$SRC_DIR"
    make clean 2>/dev/null || true
    make -j"$NPROC"

    cp fwdisplay-sender "$DIST_DIR/fwdisplay-sender-linux"
    make clean 2>/dev/null || true

    echo "[build] Done -> dist/fwdisplay-sender-linux"
    ls -lh "$DIST_DIR/fwdisplay-sender-linux"
    exit 0
fi

# --- Cross-compile from macOS ---

echo "[build] Cross-compiling Linux from macOS..."

CC="x86_64-linux-musl-gcc"
if ! command -v "$CC" &>/dev/null; then
    CC="x86_64-linux-gnu-gcc"
    if ! command -v "$CC" &>/dev/null; then
        echo "[build] ERROR: No Linux cross-compiler found"
        echo "[build] Install: brew install FiloSottile/musl-cross/musl-cross"
        exit 1
    fi
fi

if [ -z "$LINUX_SYSROOT" ]; then
    echo "[build] ERROR: Cross-compile requires LINUX_SYSROOT with X11 headers"
    echo ""
    echo "  LINUX_SYSROOT=/path/to/sysroot ./build-linux.sh"
    echo ""
    echo "  Create a sysroot from a Linux machine:"
    echo "    mkdir -p linux-sysroot/usr"
    echo "    scp -r linux:/usr/include linux-sysroot/usr/"
    echo "    scp -r linux:/usr/lib/x86_64-linux-gnu linux-sysroot/usr/lib/"
    exit 1
fi

if [ ! -d "$LINUX_SYSROOT/usr/include/X11" ]; then
    echo "[build] ERROR: Sysroot missing X11 headers at $LINUX_SYSROOT/usr/include/X11/"
    exit 1
fi

BUILD_DIR="$ROOT_DIR/build/linux-x64"
VENDOR_DIR="$ROOT_DIR/vendor"

LIBJPEG_VERSION="3.1.0"
LIBJPEG_URL="https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/${LIBJPEG_VERSION}/libjpeg-turbo-${LIBJPEG_VERSION}.tar.gz"
LIBJPEG_PREFIX="$BUILD_DIR/libjpeg"

if [ ! -f "$LIBJPEG_PREFIX/lib/libturbojpeg.a" ]; then
    echo "[build] Building libturbojpeg for Linux..."

    if [ ! -d "$VENDOR_DIR/libjpeg-turbo-${LIBJPEG_VERSION}" ]; then
        echo "[build] Downloading libjpeg-turbo ${LIBJPEG_VERSION}..."
        mkdir -p "$VENDOR_DIR"
        curl -sL "$LIBJPEG_URL" | tar xz -C "$VENDOR_DIR"
    fi

    rm -rf "$BUILD_DIR/libjpeg-build"
    mkdir -p "$BUILD_DIR/libjpeg-build"

    SIMD_FLAGS="-DWITH_SIMD=OFF"
    if command -v nasm &>/dev/null; then
        echo "[build] NASM found -- enabling SIMD"
        SIMD_FLAGS="-DWITH_SIMD=ON -DCMAKE_ASM_NASM_COMPILER=$(which nasm)"
    else
        echo "[build] NASM not found -- SIMD disabled (install: brew install nasm)"
    fi

    cmake -S "$VENDOR_DIR/libjpeg-turbo-${LIBJPEG_VERSION}" \
          -B "$BUILD_DIR/libjpeg-build" \
          -DCMAKE_SYSTEM_NAME=Linux \
          -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
          -DCMAKE_C_COMPILER="$CC" \
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
    echo "[build] libturbojpeg for Linux (cached)"
fi

echo "[build] Compiling..."
mkdir -p "$BUILD_DIR/obj"

SRCS="sender.c usbmux.c capture_linux.c"

LIBDIR="$LINUX_SYSROOT/usr/lib"
[ -d "$LINUX_SYSROOT/usr/lib/x86_64-linux-gnu" ] && LIBDIR="$LINUX_SYSROOT/usr/lib/x86_64-linux-gnu"

OBJS=()
for src in $SRCS; do
    obj="$BUILD_DIR/obj/${src%.c}.o"
    $CC -O2 -Wall -Wextra \
        -I"$LIBJPEG_PREFIX/include" \
        -I"$LINUX_SYSROOT/usr/include" \
        -c "$SRC_DIR/$src" -o "$obj"
    OBJS+=("$obj")
done

$CC "${OBJS[@]}" -o "$DIST_DIR/fwdisplay-sender-linux" \
    -L"$LIBJPEG_PREFIX/lib" -lturbojpeg \
    -L"$LIBDIR" -lX11 -lXext

echo "[build] Done -> dist/fwdisplay-sender-linux"
ls -lh "$DIST_DIR/fwdisplay-sender-linux"
