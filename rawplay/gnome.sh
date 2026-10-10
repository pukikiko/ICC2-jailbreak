#!/usr/bin/env bash
# Run a GNOME session alone, fullscreen, in a virtual (headless) 800x480 Wayland session,
# so a raw sender (rawplay/out/rawlink) can capture it and carry it to the unit. No X
# server is involved anywhere. This is rawplay/livi.sh with LIVI replaced by GNOME.
#
# Stack:
#   weston (headless backend, kiosk shell, repaint-on-capture)
#     -> gnome-shell --wayland --devkit (nested mutter)
#       -> mutter-devkit (GTK window on weston) + the GNOME session
#
# GNOME needs a compositor of its own under it too, but GNOME 50 removed the old `--nested`
# flag and the X11 backend. Nested mode is now the devkit: gnome-shell hands the session
# to /usr/libexec/mutter-devkit, whose GTK window weston's kiosk shell makes fullscreen.
# Fedora packages it separately (mutter-devkit); GNOME OS and jhbuild include it.
#
# input: mutter-devkit binds either pointer+keyboard or touch on its libei seat (its
# "Emulate touch" toggle), never both, and drops weston's wl_touch in the default mode.
# mutter-devkit-touch.so (LD_PRELOAD into gnome-shell, built by make) adds
# EI_DEVICE_CAP_TOUCH to the devkit's bind calls, so taps arrive as real touch and the
# `k` panel keys keep arriving as keys. without the shim taps do nothing; with the
# devkit's own emulate-touch toggle instead, the panel keys do nothing.
#
# The nested session is isolated on its own D-Bus (dbus-run-session), so it does not
# register org.gnome.Shell or the portals on the caller's session bus. everything the
# session bus passes down is scrubbed of the caller's desktop too (see the env block in
# start()): when this script runs from a KDE session, a dbus-activated app would
# otherwise see XDG_CURRENT_DESKTOP=KDE (gnome-control-center refuses to start) and the
# host's WAYLAND_DISPLAY/DISPLAY (its window lands on the KDE desktop).
#
# the session keeps the caller's real config by default, so gsettings, app profiles
# (firefox lives under $XDG_CONFIG_HOME now) and the gtk theme KDE configured all carry
# over. for a pristine session that looks stock GNOME (fresh dconf, no firefox profile),
# set GNOME_CONFIG_HOME, e.g. to $GNOME_LOGDIR/config.
#
# renderer: weston's GL renderer is the default here because mutter's devkit shares its
# frames as dmabufs and GTK paints them with Vulkan/GL; a pixman session cannot import
# them, so the devkit window stays black or crashes (VK_ERROR_SURFACE_LOST_KHR). pixman
# is untested for this stack: mutter's devkit is GPU-oriented, so GL is the supported
# path. weston's GL capture path comes out y-flipped on some drivers, so start rawlink
# with --flip if the image is upside down.
#
#   ./gnome.sh start           start the stack
#   ./gnome.sh stop            stop it
#
# in the emulator, `qemu/hmi.py --livi-usb` brings the gadget up and
# `rawplay/out/rawlink stream <sock> --usb --wayland GNOME_WAYLAND_SOCKET` streams this
# session; see rawplay/README.md.
#
# override with env: GNOME_WAYLAND_SOCKET (wayland-gnome), GNOME_LOGDIR,
#                    GNOME_CONFIG_HOME (default: the caller's; set it, e.g. to
#                    $GNOME_LOGDIR/config, for a pristine config/dconf/profile set),
#                    GNOME_WIDTH/GNOME_HEIGHT, GNOME_WESTON_RENDERER (gl|pixman),
#                    GNOME_WESTON_EXTRA (extra weston flags), GNOME_SHELL_EXTRA (extra
#                    gnome-shell flags, e.g. --no-x11)
set -euo pipefail

HERE="$(dirname "$(readlink -f "$0")")"
WSOCK="${GNOME_WAYLAND_SOCKET:-wayland-gnome}"
LOGDIR="${GNOME_LOGDIR:-$HOME/.local/state/gnome-weston}"
CONFIGDIR="${GNOME_CONFIG_HOME:-}"
W="${GNOME_WIDTH:-800}"
H="${GNOME_HEIGHT:-480}"
RENDERER="${GNOME_WESTON_RENDERER:-gl}"
RUNDIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
DEVPID="$LOGDIR/gnome.pid"
DEVKIT="/usr/libexec/mutter-devkit"

mkdir -p "$LOGDIR" "$RUNDIR"
if [ -n "$CONFIGDIR" ]; then
  mkdir -p "$CONFIGDIR"
fi

start() {
  # 0. nested GNOME 50 needs the devkit helper; without it gnome-shell silently becomes
  #    a display server of its own and nothing lands in the weston framebuffer.
  if [ ! -x "$DEVKIT" ]; then
    echo "gnome: $DEVKIT is missing (Fedora: sudo dnf install mutter-devkit)" >&2
    echo "gnome: without it gnome-shell cannot run nested inside weston" >&2
    exit 1
  fi

  # 1. Weston headless: the virtual session itself. --refresh-rate 0 means it repaints
  #    only when rawlink captures, and --debug authorizes those captures (the protocol is
  #    privileged). --fake-seat gives the touch module and GTK a wl_seat.
  if ! pgrep -f "socket=$WSOCK" >/dev/null; then
    echo "gnome: starting weston headless (kiosk, ${W}x${H}, $WSOCK)"
    rm -f "$RUNDIR/$WSOCK" "$RUNDIR/$WSOCK.lock" 2>/dev/null || true
    if [ ! -f "$HERE/weston-touch.so" ] && command -v make >/dev/null; then
      make -s -C "$HERE" weston-touch.so >>"$LOGDIR/weston.log" 2>&1 || true
    fi
    modules=()
    if [ -f "$HERE/weston-touch.so" ]; then
      modules=(--modules="$HERE/weston-touch.so")
    else
      echo "gnome: weston-touch.so not built (needs weston-devel), touch and panel keys will not work"
    fi
    setsid env XDG_RUNTIME_DIR="$RUNDIR" \
      weston --backend=headless --renderer="$RENDERER" --width "$W" --height "$H" \
      --refresh-rate 0 --fake-seat --debug --shell=kiosk \
      --socket="$WSOCK" -i 0 "${modules[@]}" ${GNOME_WESTON_EXTRA:-} \
      >>"$LOGDIR/weston.log" 2>&1 < /dev/null &
    for _ in $(seq 1 80); do [ -S "$RUNDIR/$WSOCK" ] && break; sleep 0.1; done
  fi

  # 2. GNOME alone, fullscreen, inside the headless session. --devkit makes mutter a
  #    nested compositor and starts mutter-devkit to show it on weston; the nested
  #    monitor is pinned to the session size so it is pixel-exact, not scaled.
  if ! pgrep -f "gnome-shell --wayland --devkit" >/dev/null; then
    if [ ! -f "$HERE/mutter-devkit-touch.so" ] && command -v make >/dev/null; then
      make -s -C "$HERE" mutter-devkit-touch.so >>"$LOGDIR/gnome-shell.log" 2>&1 || true
    fi
    preload_env=()
    if [ -f "$HERE/mutter-devkit-touch.so" ]; then
      preload_env=(LD_PRELOAD="$HERE/mutter-devkit-touch.so${LD_PRELOAD:+:$LD_PRELOAD}")
    else
      echo "gnome: mutter-devkit-touch.so not built (needs a C compiler); taps will not reach the session" >&2
    fi
    config_env=()
    if [ -n "$CONFIGDIR" ]; then
      config_env=(XDG_CONFIG_HOME="$CONFIGDIR")
    fi
    echo "gnome: starting gnome-shell (devkit) on $WSOCK"
    # the variables must be set on dbus-run-session itself: dbus-daemon hands its own
    # environment to every d-bus activated service, so a service started by the shell
    # (gnome-control-center, nautilus, ...) gets this env, not gnome-shell's. likewise the
    # caller's desktop variables have to go before the bus is up, or activated apps behave
    # as if they run inside the caller's session.
    setsid env \
      -u DESKTOP_SESSION -u SESSION_MANAGER -u DISPLAY \
      -u KDE_FULL_SESSION -u KDE_SESSION_VERSION -u KDE_SESSION_UID \
      -u KDE_APPLICATIONS_AS_SCOPE -u KDEDIRS -u KONSOLE_DBUS_SERVICE \
      -u KONSOLE_DBUS_SESSION -u KONSOLE_DBUS_WINDOW \
      -u GTK_RC_FILES -u GTK2_RC_FILES -u QT_WAYLAND_RECONNECT \
      "${preload_env[@]}" "${config_env[@]}" \
      XDG_RUNTIME_DIR="$RUNDIR" \
      XDG_CONFIG_DIRS=/etc/xdg XDG_MENU_PREFIX=gnome- \
      WAYLAND_DISPLAY="$WSOCK" \
      XDG_SESSION_TYPE=wayland XDG_SESSION_DESKTOP=gnome XDG_CURRENT_DESKTOP=GNOME \
      MUTTER_DEBUG_DUMMY_MODE_SPECS="${W}x${H}" \
      dbus-run-session -- gnome-shell --wayland --devkit ${GNOME_SHELL_EXTRA:-} \
      >>"$LOGDIR/gnome-shell.log" 2>&1 < /dev/null &
    echo $! > "$DEVPID"
    for _ in $(seq 1 100); do pgrep -x mutter-devkit >/dev/null && break; sleep 0.1; done
  fi
  echo "gnome: session up at $RUNDIR/$WSOCK"
}

stop() {
  if [ -f "$DEVPID" ]; then
    pid="$(cat "$DEVPID" 2>/dev/null || true)"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      kill -- "-$pid" 2>/dev/null || kill "$pid" 2>/dev/null || true
      sleep 1
      kill -9 -- "-$pid" 2>/dev/null || true
    fi
    rm -f "$DEVPID"
  fi
  pkill -f "gnome-shell --wayland --devkit" 2>/dev/null || true
  pkill -f "socket=$WSOCK" 2>/dev/null || true
  rm -f "$RUNDIR/$WSOCK" "$RUNDIR/$WSOCK.lock" 2>/dev/null || true
  echo "gnome: stopped"
}

case "${1:-start}" in
  start) start ;;
  stop) stop ;;
  restart) stop; sleep 1; start ;;
  *) echo "usage: $0 start|stop|restart" >&2; exit 2 ;;
esac
