#!/bin/sh
# Write the SD image to a card. Refuses to run without an explicit device and
# a typed confirmation; there is no undo.
#
# usage: scripts/flash.sh IMAGE DEVICE
#   e.g. scripts/flash.sh build/orangepi_zero2w/images/sdcard.img /dev/sdX
set -eu

[ $# -eq 2 ] || { echo "usage: $0 IMAGE DEVICE" >&2; exit 2; }
IMAGE=$1
DEV=$2

[ -f "$IMAGE" ] || { echo "flash: no such image: $IMAGE" >&2; exit 1; }
[ -b "$DEV" ] || { echo "flash: $DEV is not a block device" >&2; exit 1; }

echo "about to write:"
echo "  image:  $IMAGE ($(du -h "$IMAGE" | cut -f1))"
echo "  device: $DEV ($(lsblk -ndo SIZE,MODEL "$DEV" 2>/dev/null || echo unknown))"
printf 'type the device path again to confirm: '
read -r answer
[ "$answer" = "$DEV" ] || { echo "flash: not confirmed"; exit 1; }

sudo dd if="$IMAGE" of="$DEV" bs=4M conv=fsync status=progress
sync
echo "flash: done; on the board the release image has no console (use the debug image for the UART)"
