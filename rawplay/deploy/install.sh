#!/bin/sh
# deploy LIVI + rawlink on a postmarketOS phone (the appliance build). run it on the
# phone, from a checkout of this repo (or with --rawplay pointing at one):
#
#     ./install.sh --assets ~/livi-assets
#
# --assets is a directory holding the aarch64 LIVI release AppImage and (optionally)
# usb.img; usb.img is built from the repo when absent. everything needs sudo.
#
# steps: apk packages + the phone ui (fbkeyboard console, idle blanking), build rawlink +
# weston-touch.so + livi-cmd, /opt/livi, the ubuntu glibc chroot, LIVI config, user audio
# stack, systemd units, enable + start. idempotent; re-run after updates (it also retires
# the old livi-wakelock unit).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
LIVI_DIR=$(cd "$HERE/.." && pwd)     # rawplay/ (aka livi/)
REPO=$(cd "$HERE/../.." && pwd)

ASSETS=${LIVI_ASSETS:-$HOME/livi-assets}
LIVI_USER=${LIVI_USER:-user}
LIVI_HOME=${LIVI_HOME:-/home/$LIVI_USER}
OPT=/opt/livi
ROOTFS=$OPT/rootfs
APPIMAGE_NAME=${LIVI_APPIMAGE:-}
# the livi release targets debian trixie / ubuntu 26.04; 24.04's older libva and
# libwayland-client cannot load the bundled h264 decoder and waylandsink (black video)
UBUNTU_URL=https://cdimage.ubuntu.com/ubuntu-base/releases/26.04/release/ubuntu-base-26.04.1-base-arm64.tar.gz

SUDO=${SUDO:-}
[ -z "$SUDO" ] && { command -v sudo >/dev/null && SUDO=sudo; }
[ -z "$SUDO" ] && { command -v doas >/dev/null && SUDO=doas; }
[ -n "$SUDO" ] || { echo "need sudo or doas" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --assets) ASSETS=$2; shift 2 ;;
        --rawplay) LIVI_DIR=$2; REPO=$(cd "$2/.." && pwd); shift 2 ;;
        --user) LIVI_USER=$2; LIVI_HOME=/home/$2; shift 2 ;;
        *) echo "usage: $0 [--assets DIR] [--rawplay DIR] [--user USER]" >&2; exit 2 ;;
    esac
done

[ "$(uname -m)" = "aarch64" ] || { echo "this is the aarch64/postmarketOS build" >&2; exit 1; }

say() { printf 'install: %s\n' "$*"; }

# ---- packages ---------------------------------------------------------------------------

say "packages"
# weston is the headless wayland session the app and the sender run on; its headless
# backend and the kiosk shell are separate subpackages (without the backend weston fails
# with "failed to create compositor backend"). weston-dev is for the weston-touch.so
# module and wayland-dev carries the libwayland-client headers and wayland-scanner
# rawlink's capture needs. xkeyboard-config provides the evdev keymap weston's keyboard
# and the panel key taps use. there is no X server and no ffmpeg on this stack any more.
# postmarketos-ui-fbkeyboard is the phone's own UI (the preferred console UI, see the
# README): it is installed here, before any desktop UI is retired, so the shared
# postmarketos-base-ui package is not orphaned in between.
$SUDO apk add --quiet build-base bash weston weston-backend-headless weston-shell-kiosk \
    weston-dev wayland-dev fuse3 xkeyboard-config postmarketos-ui-fbkeyboard

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

if [ -z "$APPIMAGE_NAME" ]; then
    for f in "$ASSETS"/LIVI-*-linux-arm64.AppImage "$LIVI_DIR"/LIVI-*.AppImage; do
        [ -f "$f" ] && APPIMAGE_NAME=$f && break
    done
fi
if [ -z "$APPIMAGE_NAME" ] || [ ! -f "$APPIMAGE_NAME" ]; then
    echo "no LIVI-*-linux-arm64.AppImage found; put it in $ASSETS or set LIVI_APPIMAGE" >&2
    exit 1
fi

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

# ---- /opt/livi --------------------------------------------------------------------------

say "installing /opt/livi"
$SUDO install -d "$OPT"
$SUDO install -m 755 "$LIVI_DIR/out/rawlink" "$OPT/rawlink"
$SUDO install -m 755 "$LIVI_DIR/out/livi-cmd" "$OPT/livi-cmd"
$SUDO install -m 644 "$LIVI_DIR/weston-touch.so" "$OPT/weston-touch.so"
$SUDO install -m 644 "$ASSETS/usb.img" "$OPT/usb.img"
$SUDO install -m 755 "$HERE/livi-chroot" "$OPT/livi-chroot"
$SUDO install -m 755 "$HERE/rawlink-wait" "$OPT/rawlink-wait"
$SUDO install -m 755 "$HERE/livi-link-monitor" "$OPT/livi-link-monitor"

# ---- chroot -----------------------------------------------------------------------------

if [ ! -f "$ROOTFS/etc/os-release" ]; then
    say "ubuntu base rootfs -> $ROOTFS"
    $SUDO mkdir -p "$ROOTFS"
    wget -qO- "$UBUNTU_URL" | $SUDO tar -xz -C "$ROOTFS"
fi

if [ ! -f "$ROOTFS/.livi-deps" ]; then
    say "installing the glibc electron + wireless helper dependencies (apt)"
    $SUDO mountpoint -q "$ROOTFS/dev" || $SUDO mount --bind /dev "$ROOTFS/dev"
    $SUDO mountpoint -q "$ROOTFS/proc" || $SUDO mount -t proc proc "$ROOTFS/proc"
    $SUDO mountpoint -q "$ROOTFS/sys" || $SUDO mount -t sysfs sys "$ROOTFS/sys"
    $SUDO cp /etc/resolv.conf "$ROOTFS/etc/resolv.conf"
    # apt must not try to start NM/hostapd/bluetooth inside the chroot
    printf '#!/bin/sh\nexit 101\n' | $SUDO tee "$ROOTFS/usr/sbin/policy-rc.d" >/dev/null
    $SUDO chmod 755 "$ROOTFS/usr/sbin/policy-rc.d"
    $SUDO chroot "$ROOTFS" /bin/bash -c '
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends \
            libgtk-3-0t64 libnss3 libxss1 libxtst6 libgbm1 libasound2t64 \
            libatspi2.0-0t64 libsecret-1-0 libnotify4 libcups2t64 libdbus-1-3 libexpat1 \
            libfontconfig1 fonts-dejavu-core libxkbcommon0 libxkbcommon-x11-0 libxrandr2 \
            libxcomposite1 libxdamage1 libxfixes3 libxext6 libx11-xcb1 libxcb-dri3-0 \
            libxcb-xkb1 libxcb-shm0 libxcb-randr0 libxcb-render0 libxcb-sync1 \
            libxcb-xfixes0 libxcb-shape0 libxcb-glx0 libgl1 libegl1 libgles2 \
            libglx-mesa0 libgl1-mesa-dri libpango-1.0-0 libcairo2 libgdk-pixbuf-2.0-0 \
            libatk1.0-0t64 libatk-bridge2.0-0t64 libfuse2t64 libfuse3-4 \
            libxshmfence1 libdrm2 libssh-4 libgudev-1.0-0 \
            libva2 libva-drm2 libva-x11-2 libva-wayland2 libpulse0 pulseaudio-utils \
            python3 python3-dbus python3-gi gir1.2-glib-2.0 python3-smbus2 \
            python3-pip python3-yaml \
            gcc libc6-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
            bluez iproute2 iw rfkill hostapd dnsmasq-base procps sudo network-manager'
    # deliberately NOT libv4l-0t64: it makes the bundled v4l2 plugin load, the codec probe
    # then reports hw=true, and the app builds a v4l2h26xdec pipeline whose dmabuf output
    # the pixman nested compositor cannot take (waylandsink: "Could not bind to
    # zwp_linux_dmabuf_v1", "not-negotiated"). without it the probe reports hw=false and
    # the app uses the bundled avdec software decoders into waylandsink's shm path.
    $SUDO chroot "$ROOTFS" /bin/bash -c \
        'DEBIAN_FRONTEND=noninteractive apt-get remove -y -qq libv4l-0t64 libv4lconvert0t64 || true'
    # python 3.14 (26.04) removed the implicit loop creation in asyncio.get_event_loop();
    # livi's helper calls it at import time and would crash-loop without this shim
    $SUDO install -m 644 "$HERE/livi_asyncio_compat.py" \
        "$ROOTFS/usr/lib/python3/dist-packages/livi_asyncio_compat.py"
    $SUDO install -m 644 "$HERE/livi-asyncio-compat.pth" \
        "$ROOTFS/usr/lib/python3/dist-packages/livi-asyncio-compat.pth"
    # waylandsink hands the compositor the caps stride while videoconvert hands it
    # row-padded memory (3328 vs 3200 for 800x480 RGBx); this shim corrects the
    # wl_shm buffer stride or the video arrives sheared
    $SUDO mkdir -p "$ROOTFS/opt/livi"
    $SUDO install -m 644 "$HERE/livistride.c" "$ROOTFS/tmp/livistride.c"
    $SUDO chroot "$ROOTFS" /bin/bash -c \
        'gcc -O2 -fPIC -shared -I/usr/include/gstreamer-1.0 -I/usr/include/glib-2.0 \
         -I/usr/lib/aarch64-linux-gnu/glib-2.0/include \
         -o /opt/livi/livi-stride-fix.so /tmp/livistride.c'
    $SUDO touch "$ROOTFS/.livi-deps"
fi

$SUDO install -m 755 "$APPIMAGE_NAME" "$ROOTFS/opt/LIVI.AppImage"

# ---- LIVI config ------------------------------------------------------------------------

# the config parks wireless Android Auto on (wirelessAaEnabled): toggling it under the
# running app upsets LIVI's helper, so it comes up with the app and livi-link leaves it
# alone. once livi has run, the file is its own full settings file (carName, pairing,
# window bindings, geometry), so an existing one is only merged and never replaced; the
# deploy template is only for a missing file.
say "LIVI config (800x480 kiosk, wireless Android Auto parked on)"
$SUDO install -d "$ROOTFS$LIVI_HOME"
for d in LIVI LIVI-vnc; do
    $SUDO mkdir -p "$ROOTFS$LIVI_HOME/.config/$d"
    cfg="$ROOTFS$LIVI_HOME/.config/$d/config.json"
    if $SUDO grep -q '"wirelessAaEnabled"' "$cfg" 2>/dev/null; then
        $SUDO sed -i 's/"wirelessAaEnabled": *false/"wirelessAaEnabled": true/' "$cfg"
    else
        $SUDO install -m 644 "$HERE/livi-config.json" "$cfg"
    fi
done
$SUDO chown -R "$LIVI_USER:$LIVI_USER" "$ROOTFS$LIVI_HOME"

# the park is read at livi start, and the app keeps its old in-memory state until then:
# an upgrade applies it at the next `systemctl restart livi` (or reboot). not done here,
# because over ssh the restart would drop the connection the moment the AP takes wlan0
# (see "Wireless Android Auto" in the README).

# ---- host bluetoothd (wireless CarPlay / Android Auto) ----------------------------------

# LIVI's python helper writes bluetoothd options into its own root; from a chroot those
# never reach the host's bluetoothd, so the same drop-in is installed here. sap and midi
# keep RFCOMM channel 8 free for Android Auto's AAP and the BLE MIDI service out of the
# CarPlay EIR.
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

# the appliance has no desktop and no auto-login any more (fbkeyboard console, see step 8),
# so nothing starts the user manager at boot: without lingering, /run/user/<uid> only
# appears when somebody logs in on tty1 or over ssh, livi-chroot finds no pulse socket and
# the app comes up mute. linger keeps user@<uid> and its enabled pipewire units running
# from boot with no session.
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
        -e "s|chown -R user:user|chown -R $LIVI_USER:$LIVI_USER|" \
        "$HERE/$u.service" > "$tmp/$u.service"
    $SUDO install -m 644 "$tmp/$u.service" "/etc/systemd/system/$u.service"
done
if [ "$LIVI_USER" != "user" ]; then
    sed "s|/home/user|$LIVI_HOME|g" "$HERE/livi-chroot" > "$tmp/livi-chroot"
    $SUDO install -m 755 "$tmp/livi-chroot" "$OPT/livi-chroot"
fi
# the phone ui's idle blanking: console blank on tty1 after 2 min (screen + backlight off)
$SUDO install -m 644 "$HERE/console-blank.service" /etc/systemd/system/console-blank.service

$SUDO systemctl daemon-reload
$SUDO systemctl enable --quiet livi-link livi-weston livi rawlink console-blank
$SUDO systemctl start --no-block livi-link livi-weston livi rawlink
$SUDO systemctl start console-blank.service

say "done; check: systemctl is-active livi-link livi-weston livi rawlink fbkeyboard console-blank"
say "logs: journalctl -u livi -f   |   journalctl -u rawlink -f"
