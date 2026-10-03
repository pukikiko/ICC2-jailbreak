#!/bin/sh
# Boot-time milestone table, plus the slowest kernel initcalls and userspace
# units.
#
# usage:
#   scripts/boottime.sh qemu [--timeout S] [--images DIR]   boot qemu and measure
#   scripts/boottime.sh board SERIAL.log                    parse a board debug log
#
# The table is T0..T6 as defined in the brief: power, kernel entry, init,
# gadget bound+rawlink running, weston socket, first frame, non-blank LIVI UI.
# qemu numbers are relative only (a virtual UDC and no wire); the board table
# needs the debug image and the car (or a bench host) attached, and records
# only what the serial log can see: T5/T6 are marked unknown unless the car's
# rawplay (or a bench tests/rawsink run) acked them.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)

case "${1:-}" in
    qemu)
        shift
        exec python3 "$HERE/qemu_boot.py" "$@"
        ;;
    board)
        shift
        [ $# -ge 1 ] || { echo "usage: boottime.sh board SERIAL.log" >&2; exit 2; }
        exec python3 "$HERE/qemu_boot.py" --parse-log "$1"
        ;;
    *)
        sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
        exit 2
        ;;
esac
