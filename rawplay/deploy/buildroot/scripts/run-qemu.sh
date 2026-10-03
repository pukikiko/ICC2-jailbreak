#!/bin/sh
# Boot the qemu image for a human: same devices as scripts/qemu_boot.py, on
# stdio instead of a managed pty. Ctrl-A X quits qemu.
#
# usage: scripts/run-qemu.sh [--images DIR] [--data] [extra qemu args...]
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
IMAGES="$ROOT/build/qemu/images"

while [ $# -gt 0 ]; do
    case "$1" in
        --images) IMAGES=$2; shift 2 ;;
        --) shift; break ;;
        *) break ;;
    esac
done

[ -f "$IMAGES/disk.img" ] || {
    echo "run-qemu: no image in $IMAGES; run scripts/build.sh qemu first" >&2
    exit 1
}

exec qemu-system-aarch64 \
    -M virt -cpu max -smp 4 -m 2048 \
    -kernel "$IMAGES/Image" \
    -append "root=/dev/vda1 ro rootwait rootfstype=squashfs console=ttyAMA0,115200 systemd.journald.forward_to_console=1 random.trust_cpu=on printk.time=1 ${LIVI_QEMU_APPEND:-}" \
    -drive if=none,id=disk,format=raw,file="$IMAGES/disk.img" \
    -device virtio-blk-device,drive=disk \
    -device virtio-rng-pci \
    -display none -monitor none -no-reboot \
    -nographic "$@"
