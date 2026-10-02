#!/usr/bin/env bash
# Run LIVI alone, fullscreen, inside a TigerVNC server at exactly 800x480, so a raw sender
# (rawplay/out/rawlink) can capture it and carry it to the unit.
#
# Stack:
#   Xvnc (:9, 800x480, RFB 3.3 / security None / hextile+raw)
#     -> Weston (x11 backend, kiosk shell, pixman)   composites video + UI
#       -> livi-compositor (bundled, wayland)        places LIVI's video plane
#         -> LIVI (Electron)
#
# Running LIVI straight on X11 does not work: its GStreamer waylandsink sends the
# phone/video plane to a separate Wayland surface (wayland-0, the host's compositor), so
# Xvnc only sees the static UI and the video is black. The nested compositor keeps the
# video inside the framebuffer, and Weston (unlike plain X11 with no WM) also gives the
# app proper input focus, so touch and the knob work.
#
#   ./livi.sh start            start the stack
#   ./livi.sh stop             stop it
#
# in the emulator, `qemu/hmi.py --livi-usb` brings the gadget up and
# `rawplay/out/rawlink stream <sock> --usb --display :9` streams this display; see rawplay/README.md.
#
# override with env: APPIMAGE, LIVI_DISPLAY (9), LIVI_PORT (5909), LIVI_WAYLAND_SOCKET,
#                    LIVI_USERDATA, LIVI_LOGDIR
set -euo pipefail

HERE="$(dirname "$(readlink -f "$0")")"
APPIMAGE="${APPIMAGE:-}"
DISP="${LIVI_DISPLAY:-9}"
PORT="${LIVI_PORT:-5909}"
WSOCK="${LIVI_WAYLAND_SOCKET:-wayland-livi}"
USERDATA="${LIVI_USERDATA:-$HOME/.config/LIVI-vnc}"
LOGDIR="${LIVI_LOGDIR:-$HOME/.local/state/livi-vnc}"
W=800
H=480
XDISP=":$DISP"
RUNDIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

if [ -z "$APPIMAGE" ]; then
  for candidate in "$HERE"/LIVI-*.AppImage "$HOME/Downloads"/LIVI-*.AppImage; do
    if [ -x "$candidate" ]; then
      APPIMAGE="$candidate"
      break
    fi
  done
fi

mkdir -p "$USERDATA" "$LOGDIR"

port_open() { (echo > "/dev/tcp/127.0.0.1/$PORT") >/dev/null 2>&1; }

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

  # 2. VNC server: exact panel size, no auth, RFB 3.3, no client resize
  if ! port_open; then
    echo "livi: starting Xvnc $XDISP on port $PORT (${W}x${H})"
    setsid Xvnc "$XDISP" -geometry "${W}x${H}" -depth 16 -rfbport "$PORT" \
      -SecurityTypes None -Protocol3.3 -AcceptSetDesktopSize=0 \
      -AlwaysShared=1 -localhost=0 -noreset -br \
      >>"$LOGDIR/xvnc.log" 2>&1 < /dev/null &
    for _ in $(seq 1 50); do port_open && break; sleep 0.1; done
  fi

  # 3. Weston on top of Xvnc: composites LIVI's video plane into the X framebuffer.
  #    the weston-touch module gives it a touch device for the raw sender to drive, so the app
  #    gets real touch; without it touch is x11 mouse events (a cursor, no touch drags).
  if ! pgrep -f "socket=$WSOCK" >/dev/null; then
    echo "livi: starting Weston on $XDISP (kiosk, ${W}x${H})"
    rm -f "$RUNDIR/$WSOCK" "$RUNDIR/$WSOCK.lock" 2>/dev/null || true
    if [ ! -f "$HERE/weston-touch.so" ] && command -v make >/dev/null; then
      make -s -C "$HERE" weston-touch.so >>"$LOGDIR/weston.log" 2>&1 || true
    fi
    modules=()
    if [ -f "$HERE/weston-touch.so" ]; then
      modules=(--modules="$HERE/weston-touch.so")
    else
      echo "livi: weston-touch.so not built (needs weston-devel), touch will be a mouse"
    fi
    setsid env DISPLAY="$XDISP" XDG_RUNTIME_DIR="$RUNDIR" \
      weston --backend=x11 --width "$W" --height "$H" --shell=kiosk \
      --socket="$WSOCK" --renderer=pixman -i 0 "${modules[@]}" \
      >>"$LOGDIR/weston.log" 2>&1 < /dev/null &
    for _ in $(seq 1 80); do [ -S "$RUNDIR/$WSOCK" ] && break; sleep 0.1; done
  fi

  # 4. LIVI alone, fullscreen, inside Weston. Its bundled nested compositor puts
  #    the phone video plane in the same framebuffer Weston hands back to Xvnc.
  if ! pgrep -f "user-data-dir=$USERDATA" >/dev/null; then
    echo "livi: starting LIVI on $WSOCK"
    setsid env -u LIVI_COMPOSITOR -u LIVI_NO_COMPOSITOR \
      DISPLAY="$XDISP" XDG_RUNTIME_DIR="$RUNDIR" WAYLAND_DISPLAY="$WSOCK" \
      LIVI_KIOSK=1 \
      "$APPIMAGE" --user-data-dir="$USERDATA" --no-sandbox \
      >>"$LOGDIR/livi.log" 2>&1 < /dev/null &
  fi
  echo "livi: display $XDISP up, vnc on port $PORT"
}

stop() {
  pkill -f "user-data-dir=$USERDATA" 2>/dev/null || true
  pkill -f "socket=$WSOCK" 2>/dev/null || true
  pkill -f "[X]vnc $XDISP" 2>/dev/null || true
  rm -f "$RUNDIR/$WSOCK" "$RUNDIR/$WSOCK.lock" 2>/dev/null || true
  echo "livi: stopped"
}

case "${1:-start}" in
  start) start ;;
  stop) stop ;;
  *) echo "usage: $0 [start|stop]" >&2; exit 2 ;;
esac
