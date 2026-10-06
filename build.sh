#!/usr/bin/env bash
# build.sh - build the whole ICC2-jailbreak stack in one shot:
#
#   - all unit programs (out/): rawplay, mediaplayer, terminal, hbmenu, launcher,
#     hmi-overlay.so, iccbuttons, buswatch, syncsploit, the livi-usb compile check
#   - the host tools (rawplay/out/): rawlink, test_gadget, livi-cmd
#   - a complete homebrew stick tree under usb/homebrew/ and usb.img
#
# ICC2 (the SDK and emulator) is referenced, never copied: ICC2_DIR comes from the
# environment or is found next to this repo / in $HOME. nothing derived from the firmware
# dump is stored in this repo - the button art and the ui font are baked from ICC2's dump
# at build time when it is there, and out/, usb.img and the built stick binaries are
# git-ignored.
#
#   ./build.sh                 everything (apps + host + stick + usb.img)
#   ./build.sh --no-stick      apps and host tools only
#   ./build.sh --no-host       skip the host tools
#   ./build.sh --clean         clean first, then build from source
#   ./build.sh -j 4            parallel make jobs (default: nproc)
#   ./build.sh -h              this help
#
# environment:
#   ICC2_DIR=/path/to/ICC2     the checkout with sdk/ and qemu/
#   HMI_FONT=/path/to/font.ttf font for the ui bitmap (default: the dump's Arial, else
#                              Liberation Sans / DejaVu)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

JOBS="$(nproc 2>/dev/null || echo 1)"
DO_CLEAN=0
DO_HOST=1
DO_STICK=1

usage() { sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case "$1" in
        --clean)   DO_CLEAN=1 ;;
        --no-host) DO_HOST=0 ;;
        --no-stick) DO_STICK=0 ;;
        -j)        JOBS="${2:?build.sh: -j needs a number}"; shift ;;
        -j*)       JOBS="${1#-j}" ;;
        -h|--help) usage; exit 0 ;;
        *) echo "build.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
die() { printf 'build.sh: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- ICC2 ---
if [ -z "${ICC2_DIR:-}" ]; then
    for c in "$ROOT/../../../ICC2" "$ROOT/../../ICC2" "$ROOT/../ICC2" "$HOME/ICC2"; do
        if [ -f "$c/sdk/config.mk" ]; then
            ICC2_DIR="$(cd "$c" && pwd)"
            break
        fi
    done
fi
[ -n "${ICC2_DIR:-}" ] && [ -f "$ICC2_DIR/sdk/config.mk" ] || die \
    "ICC2 not found - set ICC2_DIR to the checkout that carries sdk/ and qemu/"
export ICC2_DIR
say "ICC2: $ICC2_DIR"

# ---------------------------------------------------------------- tools ---
for t in make clang python3; do
    command -v "$t" >/dev/null 2>&1 || die "$t not found"
done
python3 -c 'import PIL' 2>/dev/null || die "python3 Pillow not found (fonts and button art)"
if [ "$DO_STICK" = 1 ]; then
    if ! command -v guestfish >/dev/null 2>&1 && \
       ! { command -v mkfs.vfat >/dev/null 2>&1 && command -v mcopy >/dev/null 2>&1; }; then
        die "need guestfish or (mkfs.vfat + mcopy) to build usb.img"
    fi
fi
if [ ! -d "$ICC2_DIR/dump" ]; then
    echo "build.sh: warning: $ICC2_DIR/dump not found, building without the firmware dump." >&2
    echo "          the stick gets no footer-button art and the menu uses the fallback" >&2
    echo "          font, unless HMI_FONT points at a ttf." >&2
fi

# ---------------------------------------------------------------- build ---
if [ "$DO_CLEAN" = 1 ]; then
    say "clean"
    make clean
fi

say "unit programs"
make -j"$JOBS" all

if [ "$DO_HOST" = 1 ]; then
    say "host tools"
    make host
fi

if [ "$DO_STICK" = 1 ]; then
    # doom and fbdemo are built by ICC2's sdk; the stick carries them too
    if [ ! -f "$ICC2_DIR/sdk/out/fbdemo" ]; then
        say "fbdemo (ICC2 sdk)"
        make -C "$ICC2_DIR/sdk" out/fbdemo
    fi
    if [ ! -f "$ICC2_DIR/sdk/out/doom" ]; then
        say "doom (ICC2 sdk, this takes a few minutes)"
        make -C "$ICC2_DIR/sdk/doom"
    fi
    say "stick tree + usb.img"
    make -j"$JOBS" stick
fi

say "done"
echo
echo "  unit programs   out/"
[ "$DO_HOST" = 1 ] && echo "  host tools      rawplay/out/"
if [ "$DO_STICK" = 1 ]; then
    echo "  stick tree      usb/          /homebrew + /synctool, copy onto a fat32 stick"
    echo "  stick image     usb.img       (dd to a stick; ICC2's hmi.py --hbmenu attaches it)"
    echo "  installer tree  synctool/     included in the stick above; mkexploit.py builds standalone ones"
fi
