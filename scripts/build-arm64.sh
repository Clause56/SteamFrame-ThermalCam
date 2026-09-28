#!/usr/bin/env bash
# Builds an ARM64 binary for the Steam Frame from an x86-64 Ubuntu/Debian host.
# libusb is compiled statically (without udev), so the result only needs
# glibc and the bundled libopenvr_api.so on the headset.
#
#   sudo apt install g++-aarch64-linux-gnu cmake pkg-config curl bzip2
#   scripts/build-arm64.sh            -> dist/thermal-viewer-arm64/
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${WORK:-$ROOT/.build-arm64}"
SYSROOT="$WORK/sysroot"
LIBUSB_VER=1.0.27
mkdir -p "$WORK"

if [ ! -f "$SYSROOT/lib/libusb-1.0.a" ]; then
  cd "$WORK"
  curl -sSfL -o libusb.tar.bz2 \
    "https://github.com/libusb/libusb/releases/download/v$LIBUSB_VER/libusb-$LIBUSB_VER.tar.bz2"
  tar xjf libusb.tar.bz2
  cd "libusb-$LIBUSB_VER"
  ./configure --host=aarch64-linux-gnu --prefix="$SYSROOT" --disable-udev --enable-static --disable-shared
  make -j"$(nproc)" install
fi

PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR= \
  cmake -S "$ROOT" -B "$WORK/build" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/aarch64-linux-gnu.cmake" \
  -DWITH_SDL2=OFF -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
cmake --build "$WORK/build" -j"$(nproc)"

OUT="$ROOT/dist/thermal-viewer-arm64"
rm -rf "$OUT" && mkdir -p "$OUT"
cp "$WORK/build/thermal-viewer" "$WORK/build/libopenvr_api.so" "$OUT/"
cp "$ROOT/packaging/60-thermal-camera.rules" "$ROOT/packaging/install-udev-rule.sh" "$ROOT/packaging/run-in-headset.sh" "$ROOT/packaging/thermal-viewer.conf" "$ROOT/packaging/install-launcher.sh" "$ROOT/packaging/thermal-camera.png" "$OUT/"
cp "$ROOT/README.md" "$OUT/"
aarch64-linux-gnu-strip "$OUT/thermal-viewer"
echo "built $OUT"
file "$OUT/thermal-viewer"
