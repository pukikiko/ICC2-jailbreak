#!/usr/bin/env bash
# Run Waydroid alone, fullscreen, in a virtual (headless) 800x480 Wayland session, so a raw
# sender (rawplay/out/rawlink) can capture it and carry it to the unit. No X server is
# involved anywhere. This is rawplay/livi.sh with LIVI replaced by Waydroid.
#
# Stack:
#   weston (headless backend, kiosk shell, repaint-on-capture)
#     -> waydroid (Android container; surfaceflinger draws to weston as a wayland client)
#
# Waydroid needs no nested compositor of its own, unlike LIVI's bundled one and GNOME's
# devkit: its surfaceflinger is a plain Wayland client and paints the whole Android UI into
# one kiosk-fullscreen surface. `waydroid session start` is the per-user session manager
# (long-running, tracked in $LOGDIR/session.pid); `waydroid show-full-ui` is only a client
# that tells Android to show the home screen, so there is no window process to babysit.
#
# the container is root-owned and outlives the session: the script does not start it, it
# only reports the systemctl line when `waydroid session start` says it is not listening.
# one session exists per user at a time and belongs to the wayland display it was started
# on, so a session already running on another display is the caller's desktop (or another
# rawplay session) and the script refuses to steal it.
#
# readiness: `show-full-ui` needs Android's platform service and silently does nothing when
# it runs before it is up, so a session started here is first waited on for the session
# manager's "Android with user 0 is ready" line in $LOGDIR/session.log
# (WAYDROID_READY_TIMEOUT seconds, default 120).
#
# single-window mode is assumed (the waydroid default, persist.waydroid.multi_windows
# false): the Android UI fills the one surface and its resolution follows weston's output.
# `waydroid prop set persist.waydroid.width/height` overrides that, but the props live in
# the user's waydroid install, so the script leaves them alone. with multi_windows true
# every app is a surface of its own and kiosk fullscreens them on top of each other.
#
# renderer: weston's GL renderer is the default here because waydroid hands its frames to
# the compositor as dmabufs; a pixman session cannot import them and the panel stays black.
# a GPU (/dev/dri) is needed either way: android's gralloc allocates on a render node.
# weston's GL capture path comes out y-flipped on some drivers, so start rawlink with
# --flip if the image is upside down.
#
# input: waydroid takes weston's wl_touch directly, so weston-touch.so's touch device and
# panel keys arrive as real android input without a shim like GNOME's.
#
#   ./waydroid.sh start        start the stack
#   ./waydroid.sh stop         stop it
#
# in the emulator, `qemu/hmi.py --livi-usb` brings the gadget up and
# `rawplay/out/rawlink stream <sock> --usb --wayland WAYDROID_WAYLAND_SOCKET` streams this
# session; see rawplay/README.md.
#
# override with env: WAYDROID_WAYLAND_SOCKET (wayland-waydroid), WAYDROID_LOGDIR,
#                    WAYDROID_WIDTH/WAYDROID_HEIGHT, WAYDROID_WESTON_RENDERER (gl|pixman),
#                    WAYDROID_WESTON_EXTRA (extra weston flags), WAYDROID_READY_TIMEOUT
#                    (seconds to wait for android to boot)
set -euo pipefail

HERE="$(dirname "$(readlink -f "$0")")"
WSOCK="${WAYDROID_WAYLAND_SOCKET:-wayland-waydroid}"
LOGDIR="${WAYDROID_LOGDIR:-$HOME/.local/state/waydroid-weston}"
W="${WAYDROID_WIDTH:-800}"
H="${WAYDROID_HEIGHT:-480}"
RENDERER="${WAYDROID_WESTON_RENDERER:-gl}"
RUNDIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
SESSION_LOG="$LOGDIR/session.log"
SESSION_PID="$LOGDIR/session.pid"
READY_TIMEOUT="${WAYDROID_READY_TIMEOUT:-120}"
READY_LINE="Android with user 0 is ready"

mkdir -p "$LOGDIR" "$RUNDIR"

# every waydroid call has to look at the session's compositor, not the caller's desktop.
# XDG_SESSION_TYPE=wayland is what waydroid's own check wants ("XDG Session is not
# wayland" otherwise), even though weston is the display in both cases.
waydroid_env() {
  env XDG_RUNTIME_DIR="$RUNDIR" WAYLAND_DISPLAY="$WSOCK" XDG_SESSION_TYPE=wayland \
    waydroid "$@"
}

# one field out of `waydroid status` ("Session:\tRUNNING"); empty when the container
# service has no session or waydroid is not answering.
status_field() {
  local out
  out="$(waydroid_env status 2>/dev/null || true)"
  grep "^$1:" <<<"$out" | cut -d: -f2- | tr -d '[:space:]' || true
}

session_running() {
  [ "$(status_field Session)" = RUNNING ]
}

start() {
  command -v waydroid >/dev/null || { echo "waydroid: waydroid is not installed" >&2; exit 1; }
  if [[ "$(waydroid_env status 2>/dev/null || true)" == *"not initialized"* ]]; then
    echo "waydroid: waydroid is not initialized (run: sudo waydroid init)" >&2
    exit 1
  fi

  # 0. a session already running elsewhere also owns the d-bus name
  #    `id.waydro.Session` on this bus, so a second `waydroid session start` here would
  #    only log "Session is already running"; leave that session (the caller's desktop)
  #    alone and make the conflict explicit instead.
  if session_running; then
    display="$(status_field "Wayland display")"
    if [ "$display" != "$WSOCK" ]; then
      echo "waydroid: a waydroid session is already running on ${display:-another wayland display}" >&2
      echo "waydroid: stop it first (waydroid session stop)" >&2
      exit 1
    fi
  fi

  # 1. Weston headless: the virtual session itself. --refresh-rate 0 means it repaints
  #    only when rawlink captures, and --debug authorizes those captures (the protocol is
  #    privileged). --fake-seat gives the touch module and waydroid a wl_seat.
  if ! pgrep -f "socket=$WSOCK" >/dev/null; then
    echo "waydroid: starting weston headless (kiosk, ${W}x${H}, $WSOCK)"
    rm -f "$RUNDIR/$WSOCK" "$RUNDIR/$WSOCK.lock" 2>/dev/null || true
    if [ ! -f "$HERE/weston-touch.so" ] && command -v make >/dev/null; then
      make -s -C "$HERE" weston-touch.so >>"$LOGDIR/weston.log" 2>&1 || true
    fi
    modules=()
    if [ -f "$HERE/weston-touch.so" ]; then
      modules=(--modules="$HERE/weston-touch.so")
    else
      echo "waydroid: weston-touch.so not built (needs weston-devel), touch and panel keys will not work"
    fi
    setsid env XDG_RUNTIME_DIR="$RUNDIR" \
      weston --backend=headless --renderer="$RENDERER" --width "$W" --height "$H" \
      --refresh-rate 0 --fake-seat --debug --shell=kiosk \
      --socket="$WSOCK" -i 0 "${modules[@]}" ${WAYDROID_WESTON_EXTRA:-} \
      >>"$LOGDIR/weston.log" 2>&1 < /dev/null &
    for _ in $(seq 1 80); do [ -S "$RUNDIR/$WSOCK" ] && break; sleep 0.1; done
  fi

  # 2. the session manager, bound to the weston socket by WAYLAND_DISPLAY. it validates
  #    the socket before registering, so this must come after weston. its exit on failure
  #    is silent (exit 0 in the not-listening case), hence the log scan and the poll.
  started_here=no
  if ! session_running; then
    echo "waydroid: starting waydroid session on $WSOCK"
    : > "$SESSION_LOG"
    setsid env XDG_RUNTIME_DIR="$RUNDIR" WAYLAND_DISPLAY="$WSOCK" \
      XDG_SESSION_TYPE=wayland \
      waydroid session start >>"$SESSION_LOG" 2>&1 < /dev/null &
    echo $! > "$SESSION_PID"
    started_here=yes
    for _ in $(seq 1 60); do
      session_running && break
      if grep -q "container is not listening" "$SESSION_LOG"; then
        echo "waydroid: the waydroid container is not running" >&2
        echo "waydroid: start it first: sudo systemctl enable --now waydroid-container" >&2
        exit 1
      fi
      sleep 0.5
    done
    session_running || { echo "waydroid: session did not come up; see $SESSION_LOG" >&2; exit 1; }
  fi

  # 3. wait for Android before show-full-ui (see the header). a session this script did
  #    not start was already running when we got here and is left to its own log.
  if [ "$started_here" = yes ] && ! grep -q "$READY_LINE" "$SESSION_LOG"; then
    echo "waydroid: waiting up to ${READY_TIMEOUT}s for android to boot"
    for _ in $(seq 1 "$READY_TIMEOUT"); do
      grep -q "$READY_LINE" "$SESSION_LOG" && break
      sleep 1
    done
    grep -q "$READY_LINE" "$SESSION_LOG" || \
      echo "waydroid: android did not report ready; the panel may stay on the boot animation" >&2
  fi

  echo "waydroid: showing full UI on $WSOCK"
  waydroid_env show-full-ui >>"$SESSION_LOG" 2>&1 || true
  echo "waydroid: session up at $RUNDIR/$WSOCK"
}

stop() {
  # the session manager owns the container-side UI and the d-bus name; `waydroid session
  # stop` is the clean path (it stops the manager's services and releases the session).
  # when this shell is not on the bus the manager was started from (another login), the
  # pid file's SIGTERM is the fallback: it runs the same shutdown handler.
  if [ -f "$SESSION_PID" ]; then
    pid="$(cat "$SESSION_PID" 2>/dev/null || true)"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      waydroid_env session stop >/dev/null 2>&1 || true
      for _ in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
      if kill -0 "$pid" 2>/dev/null; then
        kill -- "-$pid" 2>/dev/null || kill "$pid" 2>/dev/null || true
        sleep 1
        kill -9 -- "-$pid" 2>/dev/null || true
      fi
    fi
    rm -f "$SESSION_PID"
  fi
  pkill -f "waydroid session start" 2>/dev/null || true
  pkill -f "socket=$WSOCK" 2>/dev/null || true
  rm -f "$RUNDIR/$WSOCK" "$RUNDIR/$WSOCK.lock" 2>/dev/null || true
  echo "waydroid: stopped"
}

case "${1:-start}" in
  start) start ;;
  stop) stop ;;
  restart) stop; sleep 1; start ;;
  *) echo "usage: $0 start|stop|restart" >&2; exit 2 ;;
esac
