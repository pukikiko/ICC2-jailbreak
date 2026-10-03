#!/bin/sh
# Put a homebrew stick image at /opt/livi/usb.img.
#
# A checkout that has run `make stick` (or the phone install) already has
# usb.img at the repo root: use it, unchanged, so the appliance serves the
# same launcher/hmi/rawplay payload the emulator and the phone use.
#
# A clean checkout does not (usb.img is generated and gitignored), and rawlink
# still needs an image: without one the gadget has no mass storage and the
# unit never gets /fs/usb0. Build a minimal MBR + FAT16 image containing
# homebrew/apps/rawplay.sh and the tracked stick base. This is a stand-in for
# tests and bench boards, not the real payload; README.md says to run
# `make stick` before flashing a car-bound card.
#
# usage: mkstick.sh REPO OUT HOST_DIR
set -eu

REPO=$1
OUT=$2
HOST=$3

if [ -f "$REPO/usb.img" ]; then
    echo "livi-appliance: installing existing $REPO/usb.img"
    cp "$REPO/usb.img" "$OUT"
    exit 0
fi

MTOOLS="$HOST/bin"
MKFS="$HOST/sbin/mkfs.vfat"
for t in "$MTOOLS/mcopy" "$MTOOLS/mmd" "$MKFS"; do
    [ -x "$t" ] || { echo "livi-appliance: missing $t" >&2; exit 1; }
done

# mkusb.py's layout: MBR, one partition starting at 1 MiB. The unit's
# devb-umass wants the partition table; a bare FAT gives it nothing.
SIZE_MB=16
START_LBA=2048
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

truncate -s "${SIZE_MB}M" "$tmp/part.img"
"$MKFS" -F 16 -n HOMEBREW "$tmp/part.img" >/dev/null

"$MTOOLS/mmd" -i "$tmp/part.img" ::/homebrew ::/homebrew/apps

# the tracked stick base first (boot.sh, stickwatch.sh, apps.txt, the app
# launcher scripts), then the player launcher from rawplay/, exactly the path
# the smoke test checks for
for f in "$REPO"/stick/homebrew/*; do
    [ -e "$f" ] || continue
    "$MTOOLS/mcopy" -s -o -i "$tmp/part.img" "$f" "::/homebrew/"
done
"$MTOOLS/mcopy" -o -i "$tmp/part.img" "$REPO/rawplay/rawplay.sh" \
    "::/homebrew/apps/rawplay.sh"

# prepend the MBR and the partition offset, like mkusb.build_mtools
python3 - "$tmp/part.img" "$OUT" "$START_LBA" <<'PYEOF'
import os, struct, sys

part, out, start_lba = sys.argv[1], sys.argv[2], int(sys.argv[3])
sectors = os.path.getsize(part) // 512
mbr = bytearray(512)
off = 446
mbr[off + 1:off + 4] = b'\xfe\xff\xff'
mbr[off + 4] = 0x0e  # FAT16 LBA
mbr[off + 5:off + 8] = b'\xfe\xff\xff'
mbr[off + 8:off + 12] = struct.pack('<I', start_lba)
mbr[off + 12:off + 16] = struct.pack('<I', sectors)
mbr[510:512] = b'\x55\xaa'
with open(out, 'wb') as f:
    f.write(mbr)
    f.write(bytes((start_lba - 1) * 512))
    with open(part, 'rb') as p:
        while True:
            chunk = p.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
PYEOF

echo "livi-appliance: wrote stub usb.img ($(wc -c < "$OUT") bytes)"
