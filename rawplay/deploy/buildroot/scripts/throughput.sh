#!/bin/sh
# Measure sustained vendor-interface throughput from the board to a bench
# host. Run this on the board's debug console while tests/rawsink.py runs on
# the host PC.
#
# The H618's OTG port is a mainline sunxi MUSB controller with 4 endpoints
# and 512-byte single-buffered FIFOs, no DMA (drivers/usb/musb/sunxi.c's
# dma_controller_create returns NULL) -- the brief calls this the biggest
# risk. This script produces the number that decides it.
#
# usage (board):  scripts/throughput.sh [SECONDS]
# usage (host):   python3 tests/rawsink.py --seconds SECONDS
set -eu

SECONDS=${1:-30}

[ -x /opt/livi/rawlink ] || { echo "throughput: run this on the board" >&2; exit 1; }
command -v ffmpeg >/dev/null 2>&1 || {
    echo "throughput: ffmpeg missing; use the debug image (BR2_PACKAGE_FFMPEG)" >&2
    exit 1
}

echo "throughput: starting rawsink must already be running on the host"
# the appliance service owns configfs otherwise
systemctl stop rawlink 2>/dev/null || true

# testsrc2 at the panel's own format and size, paced by ffmpeg; rawlink's
# stats print real sustained MB/s and fps (768000 B/frame)
exec /opt/livi/rawlink run --test --fps 60 --seconds "$SECONDS" --stats
