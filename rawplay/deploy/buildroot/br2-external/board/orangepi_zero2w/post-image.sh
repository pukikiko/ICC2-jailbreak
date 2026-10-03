#!/bin/sh
# Orange Pi Zero 2W post-image: build the boot FAT and the data partition,
# then the SD image.
#
# layout (GPT, like Buildroot's own orangepi boards):
#   8K..1M      raw u-boot-sunxi-with-spl.bin (sunxi boot ROM -> SPL -> TF-A
#               -> U-Boot)
#   1M          boot.vfat: Image, the DTB and boot.scr (U-Boot's bootcmd
#               loads boot.scr directly; no distro boot scan)
#   after boot  rootfs.squashfs (read-only compressed root, mounted ro)
#   after root  data.ext4 labelled livi-data (LIVI config, bluetooth
#               pairings, machine-id; commit=1, mounted noatime)
set -eu

BOARD_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="${BINARIES_DIR:-$(cd "$BOARD_DIR/../../../build/orangepi_zero2w/output/images" && pwd)}"
HOST="${HOST_DIR:?post-image runs under Buildroot}"
GENIMAGE="${HOST}/bin/genimage"
MKFS_VFAT="${HOST}/sbin/mkfs.vfat"
MCOPY="${HOST}/bin/mcopy"
MKE2FS="${HOST}/sbin/mke2fs"

DATA_SIZE=128M
BOOT_SIZE=128M

# an empty ext4 with the label the persist service looks for. 128M is plenty
# for settings and pairings and keeps the image small.
mkdir -p "${BIN}/data-root"
truncate -s "${DATA_SIZE}" "${BIN}/data.ext4"
"${MKE2FS}" -q -t ext4 -L livi-data -d "${BIN}/data-root" -F "${BIN}/data.ext4"

# the boot script U-Boot's CONFIG_BOOTCOMMAND sources. the debug image gets
# the version with the serial console and initcall_debug.
BOOTCMD="${BOARD_DIR}/boot.cmd"
if [ -n "${BR2_CONFIG:-}" ] && grep -q '^BR2_PACKAGE_LIVI_APPLIANCE_DEBUG=y' "${BR2_CONFIG}"; then
    BOOTCMD="${BOARD_DIR}/boot.cmd.debug"
fi
"${HOST}/bin/mkimage" -A arm64 -O linux -T script -C none -n "livi boot" \
    -d "${BOOTCMD}" "${BIN}/boot.scr"

# the boot FAT. FAT32 needs >= 33M; 128M also absorbs a big uncompressed
# kernel Image.
mkdir -p "${BIN}/boot-root"
cp "${BIN}/Image" "${BIN}/boot-root/Image"
cp "${BIN}"/sun50i-h618-orangepi-zero2w.dtb "${BIN}/boot-root/"
cp "${BIN}/boot.scr" "${BIN}/boot-root/boot.scr"
truncate -s "${BOOT_SIZE}" "${BIN}/boot.vfat"
"${MKFS_VFAT}" -F 32 -n boot "${BIN}/boot.vfat" >/dev/null
"${MCOPY}" -s -o -i "${BIN}/boot.vfat" "${BIN}/boot-root/"* "::/"

"${GENIMAGE}" --config "${BOARD_DIR}/genimage.cfg" \
    --inputpath "${BIN}" --outputpath "${BIN}"
