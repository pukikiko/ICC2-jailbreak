#!/bin/sh
# deploy rawlink + LIVI-Lite on a postmarketOS phone (the appliance build). run it on
# the phone, from a checkout of this repo (or with --rawplay pointing at one):
#
#     ./install.sh --livi-lite ~/LIVI-Lite
#
# LIVI-Lite (pukikiko/LIVI-Lite) is the Electron-free fork of LIVI: a Rust core, a
# Slint UI and a Rust nested compositor, all built from source against the phone's
# own musl GStreamer/Wayland. There is no AppImage, no Electron and no glibc chroot
# any more, which is what makes the appliance start in milliseconds. If no source
# tree is given, the fork is cloned next to $HOME or the repo.
#
# --assets is (still) where usb.img may live; usb.img is built from the repo when
# absent. everything needs sudo/doas.
#
# steps: apk packages + the phone ui (fbkeyboard console, idle blanking), build
# rawlink + weston-touch.so + livi-cmd, fetch + build LIVI-Lite, /opt/livi, LIVI
# config, user audio stack, systemd units, enable + start. idempotent; re-run after
# updates (it also retires the old livi-wakelock/livi-xvnc units and the chroot
# deployment).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
LIVI_DIR=$(cd "$HERE/.." && pwd)     # rawplay/ (aka livi/)
REPO=$(cd "$HERE/../.." && pwd)

ASSETS=${LIVI_ASSETS:-$HOME/livi-assets}
LIVI_USER=${LIVI_USER:-user}
LIVI_HOME=${LIVI_HOME:-/home/$LIVI_USER}
OPT=/opt/livi
# where the pukikiko/LIVI-Lite source tree lives; cloned here when missing. set
# LIVI_LITE_DIR / --livi-lite to use a checkout elsewhere, LIVI_LITE_REF to pin a
# tag or commit.
LIVI_LITE_DIR=${LIVI_LITE_DIR:-$HOME/LIVI-Lite}
LIVI_LITE_REPO=${LIVI_LITE_REPO:-https://github.com/pukikiko/LIVI-Lite}
LIVI_LITE_REF=${LIVI_LITE_REF:-}
# a pre-built stage tree (the output of deploy/livi-lite-build.sh) may be installed
# instead of building on the phone; useful for slow phones and CI
LIVI_LITE_STAGE=${LIVI_LITE_STAGE:-}

SUDO=${SUDO:-}
[ -z "$SUDO" ] && { command -v sudo >/dev/null && SUDO=sudo; }
[ -z "$SUDO" ] && { command -v doas >/dev/null && SUDO=doas; }
[ -n "$SUDO" ] || { echo "need sudo or doas" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --assets) ASSETS=$2; shift 2 ;;
        --rawplay) LIVI_DIR=$2; REPO=$(cd "$2/.." && pwd); shift 2 ;;
        --user) LIVI_USER=$2; LIVI_HOME=/home/$2; shift 2 ;;
        --livi-lite) LIVI_LITE_DIR=$2; shift 2 ;;
        --livi-stage) LIVI_LITE_STAGE=$2; shift 2 ;;
        *) echo "usage: $0 [--assets DIR] [--rawplay DIR] [--user USER]" >&2
           echo "          [--livi-lite DIR] [--livi-stage DIR]" >&2
           exit 2 ;;
    esac
done

[ "$(uname -m)" = "aarch64" ] || { echo "this is the aarch64/postmarketOS build" >&2; exit 1; }

say() { printf 'install: %s\n' "$*"; }

# some build dependencies are named differently across postmarketOS releases
# (systemd-dev vs eudev-dev, gst-plugins-bad-dev present or not); install them
# one by one so a rename cannot fail the whole deployment.
apk_optional() {
    for pkg in "$@"; do
        $SUDO apk add --quiet "$pkg" 2>/dev/null || say "note: optional package $pkg not available"
    done
}

# ---- packages ---------------------------------------------------------------------------

say "packages"
# weston is the headless wayland session the stack runs on; its headless backend and
# the kiosk shell are separate subpackages (without the backend weston fails with
# "failed to create compositor backend"). weston-dev is for the weston-touch.so
# module and wayland-dev carries the libwayland headers and wayland-scanner rawlink's
# capture needs. xkeyboard-config provides the evdev keymap weston's keyboard and the
# panel key taps use. rust/cargo build LIVI-Lite; the gst/mesa/wayland *-dev packages
# are what its build scripts link against, and the runtime plugin packages are the
# codecs and sinks LIVI's pipelines use (waylandsink, pulsesink, avdec_*/faad).
# postmarketos-ui-fbkeyboard is the phone's own UI (the preferred console UI, see the
# README): it is installed here, before any desktop UI is retired, so the shared
# postmarketos-base-ui package is not orphaned in between.
$SUDO apk add --quiet build-base bash weston weston-backend-headless weston-shell-kiosk \
    weston-dev wayland-dev xkeyboard-config postmarketos-ui-fbkeyboard \
    rust cargo pkgconf cmake perl \
    gstreamer-dev gst-plugins-base-dev \
    gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad gst-libav \
    wayland-protocols libxkbcommon-dev mesa-dev libdrm-dev \
    pulseaudio-utils bluez iproute2 iw rfkill hostapd dnsmasq sudo
apk_optional systemd-dev eudev-dev libudev-dev gst-plugins-bad-dev gst-plugins-ugly

# ---- phone ui (the preferred console UI) -------------------------------------------------

# the phone's own screen is not part of the livi pipeline, so it runs the plain framebuffer
# console with fbkeyboard rather than a desktop: a desktop session holds tty1 through
# tinydm, its session manager races the systemd user audio stack, and its idle handling
# never turned this panel's backlight off. retire any desktop UI the image had; the shared
# postmarketos-base-ui stays because fbkeyboard was installed above.
ui_old=""
for pkg in postmarketos-ui-xfce4 xfce4 xfce4-terminal xfce4-whiskermenu-plugin \
           xfce4-pulseaudio-plugin onboard \
           postmarketos-ui-sxmo-de-sway postmarketos-ui-sxmo-de-dwm \
           postmarketos-ui-sxmo-de-i3 postmarketos-ui-sxmo-de-river \
           postmarketos-ui-phosh postmarketos-ui-gnome postmarketos-ui-gnome-mobile \
           postmarketos-ui-plasma-mobile postmarketos-ui-sway postmarketos-ui-weston \
           postmarketos-ui-mate postmarketos-ui-lxqt; do
    if $SUDO apk info -e "$pkg" >/dev/null 2>&1; then
        ui_old="$ui_old $pkg"
    fi
done
if [ -n "$ui_old" ]; then
    say "phone ui: retiring the desktop UIs:$ui_old"
    $SUDO apk del --quiet $ui_old
fi

# getty owns the console, not the desktop display manager; fbkeyboard draws over it.
# note: this ends a local desktop session - run the script over ssh if you are on one.
say "phone ui: fbkeyboard console on tty1"
$SUDO systemctl disable --now tinydm >/dev/null 2>&1 || true
$SUDO systemctl enable --now getty@tty1.service
$SUDO systemctl enable --now fbkeyboard.service

# ---- assets -----------------------------------------------------------------------------

if [ ! -f "$ASSETS/usb.img" ]; then
    say "building the homebrew stick image (mkusb.py)"
    (cd "$REPO" && python3 mkusb.py)
    mkdir -p "$ASSETS"
    cp "$REPO/usb.img" "$ASSETS/usb.img"
fi

# ---- build ------------------------------------------------------------------------------

say "building rawlink, weston-touch.so and livi-cmd"
# rawlink generates and links the weston_capture_v1 client protocol (wayland-scanner and
# libwayland-client come from wayland-dev)
( cd "$LIVI_DIR" && make -s rawlink livi-cmd )
WESTON_PC=$(pkg-config --list-all 2>/dev/null | awk '/^libweston-[0-9]+ /{print $1}' | sort | tail -1)
[ -n "$WESTON_PC" ] || { echo "no libweston pkg-config file (install weston-dev)" >&2; exit 1; }
WESTON_MAJOR=${WESTON_PC#libweston-}
cc -O2 -Wall -Wextra -fPIC -DLIVI_WESTON_MAJOR="$WESTON_MAJOR" \
    $(pkg-config --cflags "$WESTON_PC" wayland-server) \
    -shared -o "$LIVI_DIR/weston-touch.so" "$LIVI_DIR/weston-touch.c" \
    $(pkg-config --libs "$WESTON_PC" wayland-server)

# ---- LIVI-Lite (built from source unless a stage tree was handed over) ------------------

if [ -n "$LIVI_LITE_STAGE" ]; then
    say "LIVI-Lite: using the pre-built stage tree $LIVI_LITE_STAGE"
    STAGE=$LIVI_LITE_STAGE
else
    if [ ! -d "$LIVI_LITE_DIR/native/livi-helperd" ]; then
        say "LIVI-Lite: cloning $LIVI_LITE_REPO"
        git clone --quiet "$LIVI_LITE_REPO" "$LIVI_LITE_DIR"
    fi
    if [ -n "$LIVI_LITE_REF" ]; then
        say "LIVI-Lite: checking out $LIVI_LITE_REF"
        git -C "$LIVI_LITE_DIR" fetch --quiet origin
        git -C "$LIVI_LITE_DIR" checkout --quiet "$LIVI_LITE_REF"
    fi
    # musl/native build: cargo builds for the phone's own target by default. a
    # native release build of the four workspaces takes a while on a phone, so the
    # stage tree is what a fast reinstall uses after the first build.
    say "LIVI-Lite: cargo build (native, $(uname -m)); this is the slow step"
    STAGE=$(mktemp -d)
    sh "$HERE/livi-lite-build.sh" "$LIVI_LITE_DIR" "$STAGE"
fi

# ---- /opt/livi --------------------------------------------------------------------------

say "installing /opt/livi"
$SUDO install -d "$OPT"
$SUDO install -m 755 "$LIVI_DIR/out/rawlink" "$OPT/rawlink"
$SUDO install -m 755 "$LIVI_DIR/out/livi-cmd" "$OPT/livi-cmd"
$SUDO install -m 644 "$LIVI_DIR/weston-touch.so" "$OPT/weston-touch.so"
$SUDO install -m 644 "$ASSETS/usb.img" "$OPT/usb.img"
$SUDO install -m 755 "$HERE/rawlink-wait" "$OPT/rawlink-wait"
$SUDO install -m 755 "$HERE/livi-link-monitor" "$OPT/livi-link-monitor"
$SUDO rm -rf "$OPT/resources"
$SUDO cp -a "$STAGE/." "$OPT/"
[ -n "$LIVI_LITE_STAGE" ] || rm -rf "$STAGE"
$SUDO chmod 755 "$OPT/livi-core" "$OPT/livi-ui"

# livi-core runs the helper as root through `sudo -n -E`; the rule is the one
# LIVI's own installer would write, but pre-seeded here so the first start can
# install its udev/wifi-ap rules without a password prompt. visudo validates
# before anything lands in /etc/sudoers.d.
say "sudoers rule for the LIVI helper"
if command -v visudo >/dev/null 2>&1; then
    sudoers_tmp=$(mktemp)
    # the helper itself (livi-core starts it as root), and pkill so a helper
    # left behind by a hard-killed core can be cleaned up on the next start
    # (livi-core runs `sudo -n pkill -f driver/livi-helperd` for that).
    PKILL=$(command -v pkill || echo /usr/bin/pkill)
    {
        printf '%s ALL=(root) NOPASSWD: SETENV: %s\n' \
            "$LIVI_USER" "$OPT/resources/driver/livi-helperd"
        printf '%s ALL=(root) NOPASSWD: %s\n' "$LIVI_USER" "$PKILL"
    } > "$sudoers_tmp"
    if visudo -c -f "$sudoers_tmp" >/dev/null 2>&1; then
        $SUDO install -d -m 0750 /etc/sudoers.d
        $SUDO install -m 0440 -o root -g root "$sudoers_tmp" /etc/sudoers.d/99-LIVI-helper
    fi
    rm -f "$sudoers_tmp"
fi

# ---- retire the AppImage/chroot deployment ---------------------------------------------

# the glibc Ubuntu chroot, the AppImage, the stride shim and the python asyncio
# shim all belonged to the Electron build; LIVI-Lite is native and none of them
# exist at runtime any more. Leave nothing behind for a stale unit to pick up.
if [ -d "$OPT/rootfs" ]; then
    say "retiring the old glibc chroot + AppImage"
    $SUDO rm -rf "$OPT/rootfs" "$OPT/livi-chroot" "$OPT/livi-stride-fix.so"
fi

# ---- LIVI config ------------------------------------------------------------------------

# the config parks wireless Android Auto on (wirelessAaEnabled): toggling it under the
# running app upsets LIVI's helper, so it comes up with the app and livi-link leaves it
# alone. once livi has run, the file is its own full settings file (carName, pairing,
# window bindings, geometry), so an existing one is only merged and never replaced; the
# deploy template is only for a missing file.
say "LIVI config (800x480 kiosk, wireless Android Auto parked on)"
$SUDO install -d "$LIVI_HOME"
for d in LIVI LIVI-vnc; do
    $SUDO mkdir -p "$LIVI_HOME/.config/$d"
    cfg="$LIVI_HOME/.config/$d/config.json"
    if $SUDO grep -q '"wirelessAaEnabled"' "$cfg" 2>/dev/null; then
        $SUDO sed -i 's/"wirelessAaEnabled": *false/"wirelessAaEnabled": true/' "$cfg"
    else
        $SUDO install -m 644 "$HERE/livi-config.json" "$cfg"
    fi
done
$SUDO chown -R "$LIVI_USER:$LIVI_USER" "$LIVI_HOME"

# the park is read at livi start, and the app keeps its old in-memory state until then:
# an upgrade applies it at the next `systemctl restart livi` (or reboot). not done here,
# because over ssh the restart would drop the connection the moment the AP takes wlan0
# (see "Wireless Android Auto" in the README).

# ---- host bluetoothd (wireless CarPlay / Android Auto) ----------------------------------

# LIVI's helper writes bluetoothd options into its own root; from a normal service it
# can reach the host's bluetoothd, but the sap/midi plugins still have to be off. sap
# keeps RFCOMM channel 8 free for Android Auto's AAP and midi takes a BLE EIR slot
# CarPlay uses.
if systemctl cat bluetooth >/dev/null 2>&1; then
    say "host bluetoothd --noplugin=sap,midi"
    bd=$(systemctl cat bluetooth 2>/dev/null | sed -n 's/^ExecStart=\([^ ]*\).*/\1/p' | head -1)
    [ -n "$bd" ] || bd=/usr/lib/bluetooth/bluetoothd
    $SUDO mkdir -p /etc/systemd/system/bluetooth.service.d
    printf '[Service]\nExecStart=\nExecStart=%s --noplugin=sap,midi\n' "$bd" |
        $SUDO tee /etc/systemd/system/bluetooth.service.d/livi-no-sap.conf >/dev/null
    $SUDO systemctl daemon-reload
    $SUDO systemctl restart bluetooth || true
fi

# ---- user audio stack -------------------------------------------------------------------

# postmarketos desktop sessions autostart /usr/libexec/pipewire-launcher (it pkills and
# execs pipewire + wireplumber + pipewire-pulse) while the enabled systemd user socket
# units bind the same pipewire-0/pulse sockets at login. the launcher steals the core
# socket path while systemd's pipewire.socket holds it; the systemd service then fails to
# take the lock and hits its start limit, and the path can be left pointing at a dead
# listener: every client, pulse included, gets ECONNREFUSED, so LIVI has no audio at all.
# hide the autostart so the systemd user stack is the only owner; it is supervised
# (Restart=on-failure) and the README's repair (`systemctl --user restart pipewire
# pipewire-pulse wireplumber`) then works.
if [ -e /etc/xdg/autostart/pipewire.desktop ]; then
    say "user audio stack: hiding the pipewire autostart launcher (systemd owns it)"
    $SUDO install -d "$LIVI_HOME/.config/autostart"
    $SUDO install -m 644 "$HERE/pipewire.desktop" "$LIVI_HOME/.config/autostart/pipewire.desktop"
    $SUDO chown -R "$LIVI_USER:$LIVI_USER" "$LIVI_HOME/.config/autostart"
fi

LIVI_UID=$(id -u "$LIVI_USER" 2>/dev/null || true)
[ -n "$LIVI_UID" ] || { echo "no such user: $LIVI_USER" >&2; exit 1; }

# the appliance has no desktop and no auto-login any more (fbkeyboard console, see step 8),
# so nothing starts the user manager at boot: without lingering, /run/user/<uid> only
# appears when somebody logs in on tty1 or over ssh, and LIVI's pulse calls find no socket.
# linger keeps user@<uid> and its enabled pipewire units running from boot with no session.
if [ -n "$LIVI_UID" ]; then
    say "user audio stack: linger for $LIVI_USER (pipewire without a login session)"
    $SUDO loginctl enable-linger "$LIVI_USER" || true
    # enable-linger takes effect at the next boot; start the manager now too, so a headless
    # install (nobody has logged in) gets the stack for the running livi
    $SUDO systemctl start "user@$LIVI_UID.service" 2>/dev/null || true
    i=0
    while [ ! -S "/run/user/$LIVI_UID/bus" ] && [ "$i" -lt 10 ]; do
        sleep 1
        i=$((i + 1))
    done
fi

# bring that stack up now if the user manager is reachable (linger starts it if it is not);
# killing the launcher's orphan processes first is what makes this repair an already-broken
# install: they hold the pipewire-0 lock the systemd service needs, and pipewire does not
# remove a stale socket path while a live process still has the socket bound.
if [ -n "$LIVI_UID" ] && [ -S "/run/user/$LIVI_UID/bus" ]; then
    say "user audio stack: systemd user units own pipewire/pulse/wireplumber"
    $SUDO pkill -u "$LIVI_USER" -fx /usr/bin/pipewire-pulse || true
    $SUDO pkill -u "$LIVI_USER" -fx /usr/bin/wireplumber || true
    $SUDO pkill -u "$LIVI_USER" -fx /usr/bin/pipewire || true
    sleep 1
    $SUDO rm -f "/run/user/$LIVI_UID/pipewire-0" "/run/user/$LIVI_UID/pipewire-0-manager" \
        "/run/user/$LIVI_UID/pipewire-0.lock" "/run/user/$LIVI_UID/pipewire-0-manager.lock" \
        "/run/user/$LIVI_UID/pulse/native" "/run/user/$LIVI_UID/pulse/pid"
    $SUDO -u "$LIVI_USER" env XDG_RUNTIME_DIR="/run/user/$LIVI_UID" \
        systemctl --user reset-failed pipewire.socket pipewire-pulse.socket \
        pipewire.service pipewire-pulse.service wireplumber.service || true
    $SUDO -u "$LIVI_USER" env XDG_RUNTIME_DIR="/run/user/$LIVI_UID" \
        systemctl --user enable --now pipewire.socket pipewire-pulse.socket \
        wireplumber.service || true
fi

# ---- units ------------------------------------------------------------------------------

say "systemd units"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# livi-wakelock was replaced by livi-link (the charger/rawlink power state machine); retire
# it on an upgrade so only one unit writes the wake lock and the sxmo flag. livi-xvnc is the
# retired X server: the stack is wayland-only now (the headless weston session), so the
# unit, its X socket and tigervnc are gone.
if $SUDO systemctl cat livi-wakelock >/dev/null 2>&1; then
    say "retiring the old livi-wakelock unit"
    $SUDO systemctl disable --now livi-wakelock >/dev/null 2>&1 || true
    $SUDO rm -f /etc/systemd/system/livi-wakelock.service
fi
if $SUDO systemctl cat livi-xvnc >/dev/null 2>&1; then
    say "retiring the old livi-xvnc unit (wayland-only now)"
    $SUDO systemctl disable --now livi-xvnc >/dev/null 2>&1 || true
    $SUDO rm -f /etc/systemd/system/livi-xvnc.service
fi
for u in livi-link livi-weston livi rawlink; do
    sed -e "s|/home/user|$LIVI_HOME|g" -e "s|^User=user$|User=$LIVI_USER|" \
        -e "s|/run/user/10000|/run/user/$LIVI_UID|g" \
        -e "s|chown -R user:user|chown -R $LIVI_USER:$LIVI_USER|" \
        "$HERE/$u.service" > "$tmp/$u.service"
    $SUDO install -m 644 "$tmp/$u.service" "/etc/systemd/system/$u.service"
done
# the phone ui's idle blanking: console blank on tty1 after 2 min (screen + backlight off)
$SUDO install -m 644 "$HERE/console-blank.service" /etc/systemd/system/console-blank.service

$SUDO systemctl daemon-reload
$SUDO systemctl enable --quiet livi-link livi-weston livi rawlink console-blank
# restart, not start: on an in-place upgrade the old processes are still running and
# `systemctl start` on an active unit is a no-op, so the new binaries and the new
# livi-weston backend would never take effect. restart starts them on a fresh install too.
$SUDO systemctl restart --no-block livi-link livi-weston livi rawlink
$SUDO systemctl start console-blank.service

say "done; check: systemctl is-active livi-link livi-weston livi rawlink fbkeyboard console-blank"
say "logs: journalctl -u livi -f   |   journalctl -u rawlink -f"
