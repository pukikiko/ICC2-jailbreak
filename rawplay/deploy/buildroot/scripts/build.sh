#!/bin/sh
# Build the appliance image with one command per board. Fetches and pins
# Buildroot, adds this br2-external tree, loads the defconfig and builds.
#
# usage: scripts/build.sh BOARD [debug]
#   BOARD   qemu | orangepi_zero2w
#   debug   use the *_debug defconfig (serial console, dropbear, ffmpeg)
#
# results:
#   qemu             -> build/qemu/images/{Image,disk.img}
#   orangepi_zero2w  -> build/orangepi_zero2w/images/sdcard.img
set -eu

# Pinned release. 2026.08 (not the 2026.02 LTS) because its weston 15 is the
# first in this range with the headless backend's --fake-seat; weston 14 has
# no seat at all and breaks weston-touch/Electron. See br2-external
# configs and README.md.
BR_VERSION=2026.08
BR_URL="https://buildroot.org/downloads/buildroot-${BR_VERSION}.tar.gz"
BR_SHA256=d678e810abf877d04513e03ca2c99f992dd49118b9c2e18d6e25f5f58fa8c5cd

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
EXTERNAL="$ROOT/br2-external"
BUILD="$ROOT/build"
DL="$BUILD/downloads"
SRC="$BUILD/buildroot-$BR_VERSION"

[ $# -ge 1 ] || { sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
BOARD=$1
VARIANT=${2:-release}

case "$BOARD" in
    qemu)
        DEFCONFIG=qemu_aarch64_virt_defconfig
        OUT="$BUILD/qemu"
        ;;
    orangepi_zero2w)
        if [ "$VARIANT" = debug ]; then
            DEFCONFIG=orangepi_zero2w_debug_defconfig
            OUT="$BUILD/orangepi_zero2w-debug"
        else
            DEFCONFIG=orangepi_zero2w_defconfig
            OUT="$BUILD/orangepi_zero2w"
        fi
        ;;
    *)
        echo "unknown board: $BOARD (qemu | orangepi_zero2w)" >&2
        exit 2
        ;;
esac

mkdir -p "$BUILD" "$DL"
if [ ! -d "$SRC" ]; then
    if [ ! -f "$DL/buildroot-$BR_VERSION.tar.gz" ]; then
        echo "build: fetching Buildroot $BR_VERSION"
        curl -fL -o "$DL/buildroot-$BR_VERSION.tar.gz" "$BR_URL"
    fi
    echo "$BR_SHA256  $DL/buildroot-$BR_VERSION.tar.gz" | sha256sum -c - >/dev/null
    tar -xzf "$DL/buildroot-$BR_VERSION.tar.gz" -C "$BUILD"
fi

echo "build: $BOARD ($VARIANT) -> $OUT"
make -C "$SRC" O="$OUT" BR2_EXTERNAL="$EXTERNAL" "$DEFCONFIG"
make -C "$SRC" O="$OUT" BR2_EXTERNAL="$EXTERNAL" -j"$(nproc)"

echo
echo "build: done. images in $OUT/images:"
ls -la "$OUT/images" | sed 's/^/  /'
if [ "$BOARD" = orangepi_zero2w ]; then
    echo "build: flash with scripts/flash.sh $OUT/images/sdcard.img /dev/sdX"
else
    echo "build: run with scripts/run-qemu.sh, measure with scripts/boottime.sh qemu"
fi
