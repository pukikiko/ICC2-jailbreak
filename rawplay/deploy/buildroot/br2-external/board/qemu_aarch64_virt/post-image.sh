#!/bin/sh
# qemu needs no SD image of its own, but it does need the same disk shape the
# board has: build the labelled ext4 data partition, then one GPT image with
# the squashfs root as p1 and the data as p2, so the kernel command line can
# say root=/dev/vda1 without depending on how qemu enumerates virtio devices.
set -eu

BOARD_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="${BINARIES_DIR:?post-image runs under Buildroot}"
HOST="${HOST_DIR:?post-image runs under Buildroot}"

mkdir -p "${BIN}/data-root"
truncate -s 128M "${BIN}/data.ext4"
"${HOST}/sbin/mke2fs" -q -t ext4 -L livi-data -d "${BIN}/data-root" \
    -F "${BIN}/data.ext4"

"${HOST}/bin/genimage" --config "${BOARD_DIR}/genimage.cfg" \
    --inputpath "${BIN}" --outputpath "${BIN}"
