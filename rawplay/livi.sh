#!/usr/bin/env bash
# Run LIVI-Lite alone, fullscreen, in a virtual (headless) 800x480 Wayland session, so a
# raw sender (rawplay/out/rawlink) can capture it and carry it to the unit. No X server is
# involved anywhere.
#
# Stack:
#   weston (headless backend, kiosk shell, repaint-on-capture)
#     -> livi-compositor (started by livi-core)   places LIVI's video plane
#       -> livi-ui (Slint) + livi-gst-host
#
# LIVI-Lite is the Electron-free fork: livi-core starts its own nested compositor, the
# Slint UI, the GStreamer video host and the helper, all natively. The nested compositor
# is still needed because the GStreamer waylandsink sends the video plane to a separate
# Wayland surface that has to stay inside the framebuffer the sender captures.
#
# the installed deployment (deploy/install.sh) puts the runtime at /opt/livi. To run a
# source build instead, point LIVI_CORE at the cargo output and set LIVI_ROOT:
#
#   LIVI_CORE=~/LIVI-Lite/native/livi-helperd/target/release/livi-core ./livi.sh start
#
#   ./livi.sh start            start the stack
#   ./livi.sh stop             stop it
#
# in the emulator, `qemu/hmi.py --livi-usb` brings the gadget up and
# `rawplay/out/rawlink stream <sock> --usb --wayland LIVI_WAYLAND_SOCKET` streams this
# session; see rawplay/README.md.
#
# override with env: LIVI_CORE (binary), LIVI_RESOURCES (installed resources dir, default
# /opt/livi/resources), LIVI_ROOT (source checkout for the repo layout),
# LIVI_WAYLAND_SOCKET (wayland-livi), LIVI_USERDATA, LIVI_LOGDIR,
# LIVI_WESTON_RENDERER (pixman|gl), LIVI_WESTON_EXTRA (extra weston flags)
set -euo pipefail

HERE="$(dirname "$(readlink -f "$0")")"
LIVI_CORE="${LIVI_CORE:-${LIVI_ROOT:-}/native/livi-helperd/target/release/livi-core}"
[ -x "$LIVI_CORE" ] || LIVI_CORE=/opt/livi/livi-core
if [ -z "${LIVI_ROOT:-}" ] && [ -z "${LIVI_RESOURCES:-}" ]; then
  LIVI_RESOURCES=/opt/livi/resources
fi
WSOCK="${LIVI_WAYLAND_SOCKET:-wayland-livi}"
USERDATA="${LIVI_USERDATA:-$HOME/.config/LIVI-vnc}"
LOGDIR="${LIVI_LOGDIR:-$HOME/.local/state/livi-vnc}"
W=800
H=480
RENDERER="${LIVI_WESTON_RENDERER:-pixman}"
RUNDIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

mkdir -p "$USERDATA" "$LOGDIR" "$RUNDIR"

start() {
  [ -x "$LIVI_CORE" ] || { echo "livi: no livi-core binary (set LIVI_CORE)" >&2; exit 1; }

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
  #    privileged). --fake-seat gives the touch module and the UI a wl_seat.
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

  # 3. LIVI-Lite alone, fullscreen, inside the headless session. livi-core starts its
  #    nested compositor and the UI itself and puts the phone video plane in the same
  #    framebuffer rawlink captures.
  if ! pgrep -f "livi-core" >/dev/null; then
    echo "livi: starting LIVI-Lite ($LIVI_CORE) on $WSOCK"
    setsid env XDG_RUNTIME_DIR="$RUNDIR" WAYLAND_DISPLAY="$WSOCK" \
      LIVI_KIOSK=1 \
      ${LIVI_RESOURCES:+LIVI_RESOURCES="$LIVI_RESOURCES"} \
      ${LIVI_ROOT:+LIVI_ROOT="$LIVI_ROOT"} \
      "$LIVI_CORE" >>"$LOGDIR/livi.log" 2>&1 < /dev/null &
  fi
  echo "livi: session up at $RUNDIR/$WSOCK"
}

stop() {
  pkill -f "livi-core" 2>/dev/null || true
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
