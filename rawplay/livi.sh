#!/usr/bin/env bash
# Run LIVI alone, fullscreen, in a virtual (headless) 800x480 Wayland session, so a raw
# sender (rawplay/out/rawlink) can capture it and carry it to the unit. No X server is
# involved anywhere.
#
# Stack:
#   weston (headless backend, kiosk shell, repaint-on-capture)
#     -> livi-compositor (bundled, wayland)        places LIVI's video plane
#       -> LIVI (Electron)
#
# LIVI needs a compositor of its own under it: its GStreamer waylandsink sends the
# phone/video plane to a separate Wayland surface, and the nested compositor keeps that
# inside the framebuffer the sender captures.
#
# renderer: LIVI 8.3.0 works with the CPU (pixman) session the phone runs. LIVI 9.0.0
# forces its inner app to Wayland (--ozone-platform=wayland) and its GPU process hands
# the nested compositor dmabufs; a pixman session's EGL is software and cannot import
# them, so the app dies with `create_immed ... invalid wl_buffer` and the panel stays
# black. on a machine with a working GPU run LIVI_WESTON_RENDERER=gl (or just try it:
# see the troubleshooting table in deploy/README.md). on NVIDIA that GL session's capture
# comes out bottom-up (weston's async GL path), so start rawlink with --flip too.
#
#   ./livi.sh start            start the stack
#   ./livi.sh stop             stop it
#
# in the emulator, `qemu/hmi.py --livi-usb` brings the gadget up and
# `rawplay/out/rawlink stream <sock> --usb --wayland LIVI_WAYLAND_SOCKET` streams this
# session; see rawplay/README.md.
#
# override with env: APPIMAGE, LIVI_WAYLAND_SOCKET (wayland-livi), LIVI_USERDATA,
#                    LIVI_LOGDIR, LIVI_WESTON_RENDERER (pixman|gl), LIVI_WESTON_EXTRA
#                    (extra weston flags)
set -euo pipefail

HERE="$(dirname "$(readlink -f "$0")")"
APPIMAGE="${APPIMAGE:-}"
WSOCK="${LIVI_WAYLAND_SOCKET:-wayland-livi}"
USERDATA="${LIVI_USERDATA:-$HOME/.config/LIVI-vnc}"
LOGDIR="${LIVI_LOGDIR:-$HOME/.local/state/livi-vnc}"
W=800
H=480
RENDERER="${LIVI_WESTON_RENDERER:-pixman}"
RUNDIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

if [ -z "$APPIMAGE" ]; then
  for candidate in "$HERE"/LIVI-*.AppImage "$HOME/Downloads"/LIVI-*.AppImage; do
    if [ -x "$candidate" ]; then
      APPIMAGE="$candidate"
      break
    fi
  done
fi

mkdir -p "$USERDATA" "$LOGDIR" "$RUNDIR"

start() {
  [ -n "$APPIMAGE" ] && [ -x "$APPIMAGE" ] || { echo "livi: no executable AppImage (set APPIMAGE)" >&2; exit 1; }

  # 1. private config for this instance: kiosk, 800x480, pinned to the top-left
  if [ ! -f "$USERDATA/config.json" ]; then
    cat > "$USERDATA/config.json" <<JSON
{
  "mainScreenWidth": $W,
  "mainScreenHeight": $H,
  "mainScreenBounds": { "x": 0, "y": 0, "width": $W, "height": $H },
  "kiosk": { "main": true }
}
JSON
  fi

  # 2. Weston headless: the virtual session itself. --refresh-rate 0 means it repaints
  #    only when rawlink captures, and --debug authorizes those captures (the protocol is
  #    privileged). --fake-seat gives the touch module and electron a wl_seat.
  if ! pgrep -f "socket=$WSOCK" >/dev/null; then
    echo "livi: starting weston headless (kiosk, ${W}x${H}, wayland-$WSOCK)"
    rm -f "$RUNDIR/$WSOCK" "$RUNDIR/$WSOCK.lock" 2>/dev/null || true
    if [ ! -f "$HERE/weston-touch.so" ] && command -v make >/dev/null; then
      make -s -C "$HERE" weston-touch.so >>"$LOGDIR/weston.log" 2>&1 || true
    fi
    modules=()
    if [ -f "$HERE/weston-touch.so" ]; then
      modules=(--modules="$HERE/weston-touch.so")
    else
      echo "livi: weston-touch.so not built (needs weston-devel), touch and panel keys will not work"
    fi
    setsid env XDG_RUNTIME_DIR="$RUNDIR" \
      weston --backend=headless --renderer="$RENDERER" --width "$W" --height "$H" \
      --refresh-rate 0 --fake-seat --debug --shell=kiosk \
      --socket="$WSOCK" -i 0 "${modules[@]}" ${LIVI_WESTON_EXTRA:-} \
      >>"$LOGDIR/weston.log" 2>&1 < /dev/null &
    for _ in $(seq 1 80); do [ -S "$RUNDIR/$WSOCK" ] && break; sleep 0.1; done
  fi

  # 3. LIVI alone, fullscreen, inside the headless session. Its bundled nested compositor
  #    puts the phone video plane in the same framebuffer rawlink captures.
  if ! pgrep -f "user-data-dir=$USERDATA" >/dev/null; then
    echo "livi: starting LIVI on $WSOCK"
    setsid env -u LIVI_COMPOSITOR -u LIVI_NO_COMPOSITOR \
      XDG_RUNTIME_DIR="$RUNDIR" WAYLAND_DISPLAY="$WSOCK" \
      LIVI_KIOSK=1 \
      "$APPIMAGE" --user-data-dir="$USERDATA" --no-sandbox \
      >>"$LOGDIR/livi.log" 2>&1 < /dev/null &
  fi
  echo "livi: session up at $RUNDIR/$WSOCK"
}

stop() {
  pkill -f "user-data-dir=$USERDATA" 2>/dev/null || true
  pkill -f "socket=$WSOCK" 2>/dev/null || true
  rm -f "$RUNDIR/$WSOCK" "$RUNDIR/$WSOCK.lock" 2>/dev/null || true
  echo "livi: stopped"
}

case "${1:-start}" in
  start) start ;;
  stop) stop ;;
  restart) stop; sleep 1; start ;;
  *) echo "usage: $0 start|stop|restart" >&2; exit 2 ;;
esac
