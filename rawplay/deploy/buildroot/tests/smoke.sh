#!/bin/sh
# qemu end-to-end smoke test: boots the image, waits for T6, checks the
# gadget, the stick, the units and LIVI's helper, and prints the boot-time
# table. Non-zero on any failure, with the last console lines.
#
# dummy_hcd is a virtual host controller in the same guest and has been seen
# to oops its timer once under sustained bulk traffic (a harness flake, not
# the appliance); the console log then says "Kernel panic". Retry once for
# that case only, so a real failure still fails.
#
# usage: tests/smoke.sh [--images DIR] [--timeout S]
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
LOG=${SMOKE_LOG:-"$ROOT/build/smoke-console.log"}

attempt=1
while :; do
    if python3 "$ROOT/scripts/qemu_boot.py" --checks --log "$LOG" "$@"; then
        exit 0
    fi
    if [ "$attempt" -lt 2 ] && grep -aq 'Kernel panic' "$LOG" 2>/dev/null; then
        echo "smoke: dummy_hcd kernel panic in the guest, retrying once" >&2
        attempt=$((attempt + 1))
        continue
    fi
    exit 1
done
