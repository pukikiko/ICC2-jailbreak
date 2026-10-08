# LIVI-Lite + rawlink on a postmarketOS phone (the appliance build)

This is the deployment that turns a postmarketOS phone into the head-unit side of the raw
livi link: the phone runs **LIVI-Lite** (pukikiko/LIVI-Lite, the Electron-free fork of
LIVI) and `rawlink`, its USB port enumerates as the car's USB gadget (vendor video
interface + the homebrew stick), and the unit's 800x480 panel becomes LIVI's display with
touch/knob/button coming back up the same wire. Everything is a systemd unit that starts
at boot and restarts on its own. One of them, `livi-link`, is the power state machine: the
charger decides the radios and LIVI's lifecycle, rawlink's own log decides the CPU idle
depth - unplugged means radios off, LIVI stopped and deep idle, charging with a player on
the link means shallow CPU idle (see "The rawplay link state"). Wireless Android Auto is
parked on in LIVI's config and left alone: toggling it under the running app upsets LIVI's
helper.

**LIVI-Lite is built from source on the phone** (native aarch64 musl, no AppImage and no
glibc chroot), and it starts in milliseconds: livi-core, the nested compositor, the Slint
UI and the Rust helper are plain binaries, where the old deployment paid an AppImage
extraction plus a full Electron cold start. The boot order is unchanged; only the last
step got native.

Reference hardware: **Xiaomi Mi A1 (qcom-msm8953, "tissot")**, postmarketOS edge with
systemd. The virtual display is 800x480 to match the unit's panel, and it is a **virtual
wayland session, not X**: weston's headless backend composites everything, rawlink captures
the output over the `weston_capture_v1` wayland protocol, and touch/panel keys go back
through the `weston-touch` module's socket. No X server, no XTest, no x11grab and no ffmpeg
anywhere in the stack. The phone's own screen runs the **fbkeyboard console UI**, the
preferred UI for the appliance phone (see "The phone's own UI"): a plain framebuffer
console with an on-screen keyboard, screen and backlight off when idle. The stack itself
is generic: any aarch64 postmarketOS phone with a configfs/functionfs-capable UDC works.

```
 phone (postmarketOS, musl)                          car (i.MX31, QNX, usb host)
 ┌──────────────────────────────────────────┐        ┌──────────────────────────┐
 │ LIVI-Lite (native aarch64 musl)          │        │                          │
 │  livi-core ─ livi-compositor ─ livi-ui   │        │                          │
 │      │ wayland (nested compositor)        │        │                          │
 │ weston headless (kiosk, pixman, 800x480) │        │                          │
 │      │ weston_capture_v1 (shm)            │        │                          │
 │ rawlink stream ── functionfs vendor bulk ├───────►│ rawplay → IPU scanout    │
 │ rawlink gadget ── mass_storage (usb.img) │◄───────┤ /fs/usb0/homebrew/apps   │
 │      ▲ touch/knob/button (LI messages)   │        │ touch/knob/buttons       │
 └──────┴───────────────────────────────────┘        └──────────────────────────┘
        USB-C (device)               USB-A (host)
```

## Why there is no chroot any more

LIVI's release builds were glibc Electron bundles and postmarketOS is musl, so the old
deployment ran the AppImage inside a small Ubuntu base rootfs (`livi-chroot`). LIVI-Lite
removed the reason for both: it is a Rust core, a Slint UI and a Rust nested compositor,
and `install.sh` builds all of it from source for the phone's own ABI. The four binaries
are ordinary musl binaries linked against the system GStreamer, Wayland and EGL, so there
is no rootfs to unpack, no mount dance, no FUSE AppImage and no Electron/Chromium cold
start. That is also where the boot-time win comes from: `livi.service` starts
`/opt/livi/livi-core` directly and it is listening and painting the UI in milliseconds.

The bundled-GStreamer pitfalls the old deployment had to work around are gone with the
bundle: the video-plane stride shim (`livistride.c`) existed because the AppImage's own
GStreamer and waylandsink disagreed about the row padding - the system GStreamer the Rust
`livi-gst-host` links against is self-consistent. The python 3.14 asyncio shim existed for
the packaged python helper; LIVI-Lite's helper (`livi-helperd`) is Rust. The optional
packages in `livi-config.json` stay dismissed: the phone's radios are still driven by the
helper through hostapd/BlueZ, not by a desktop stack.

## The phone's own UI (fbkeyboard)

The phone's own screen is not part of the livi pipeline, and a full desktop on it fought
the appliance: the desktop session holds `tty1` through tinydm, its session manager races
the systemd user audio stack (see "Who owns PipeWire"), and its idle handling never turned
this panel's backlight off. The appliance phone therefore runs the plain **framebuffer
console with fbkeyboard** (`postmarketos-ui-fbkeyboard`) as its preferred UI: a login
console plus an on-screen keyboard for the touchscreen, and nothing between boot and the
console.

The idle blanking is what makes the console usable as an appliance, and it works on this
panel: the kernel's console blank calls `fb_blank`, the `panel-otm1911` driver disables the
WLED backlight with it (`/sys/class/backlight/backlight/bl_power` goes to 4), and any input
- including fbkeyboard's injected key events - unblanks again. `console-blank.service` sets
a 2-minute timeout; step 8 has the manual commands, and `install.sh` retires xfce4/sxmo (or
any other UI the image shipped with) and sets all of it up.

## Files here

| file | what it is |
|---|---|
| `install.sh` | automated deployment on the phone (packages, phone UI, rawlink, LIVI-Lite build, units) |
| `livi-lite-build.sh` | the shared source-build recipe: builds the four LIVI-Lite workspaces and stages the installed layout (also called by the Buildroot package) |
| `livi-config.json` | LIVI settings: 800x480 kiosk, wireless AA parked on, and the optional-package dialog dismissed |
| `livi-link-monitor` | the power state machine: the charger sets the radios and starts/stops LIVI, rawlink's log sets the CPU idle depth (see "The rawplay link state") |
| `livi-cmd.c` | one-line client for LIVI's helper control socket, for toggling wireless AA by hand; goes to `/opt/livi/livi-cmd` |
| `livi-link.service` | runs `livi-link-monitor` as root at boot, restart-forever |
| `pipewire.desktop` | xdg autostart override that hides the desktop's pipewire-launcher so the systemd user units own audio (see "Audio") |
| `livi-weston.service` | weston headless/kiosk/pixman at 800x480 (repaint-on-capture, `--debug` for capture authorization); hosts the app, the video plane and the `weston-touch` module |
| `livi.service` | runs `/opt/livi/livi-core` (LIVI-Lite) on `wayland-livi`; no chroot |
| `rawlink.service` | `rawlink run`: functionfs gadget + mass storage + headless-weston wayland sender |
| `rawlink-wait` | forces device mode, waits for the UDC, then execs rawlink (the unit's main process) |
| `console-blank.service` | the phone UI's idle blanking: console blank on `tty1` (screen + backlight off) after 2 min |

## Deploy, step by step

Run as a user with sudo on the phone. `install.sh` does all of this; the steps are here
for when something needs doing by hand.

### 1. Packages

     sudo apk add build-base bash weston weston-backend-headless weston-shell-kiosk \
                  weston-dev wayland-dev xkeyboard-config postmarketos-ui-fbkeyboard \
                  rust cargo pkgconf cmake perl \
                  gstreamer-dev gst-plugins-base-dev \
                  gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad gst-libav \
                  wayland-protocols libxkbcommon-dev mesa-dev libdrm-dev \
                  pulseaudio-utils bluez iproute2 iw rfkill hostapd dnsmasq sudo
     # names that move between postmarketOS releases; install what exists
     sudo apk add systemd-dev gst-plugins-bad-dev || true

* `postmarketos-ui-fbkeyboard` is the phone's own UI, the preferred console UI (see "The
  phone's own UI"). install it **before** removing a desktop UI, so the shared
  `postmarketos-base-ui` package is never orphaned in between (step 8).
* `weston`/`weston-dev` are the headless session and the headers for the `weston-touch`
  module; `weston-backend-headless` and `weston-shell-kiosk` are separate subpackages
  (without the headless backend weston fails with
  `failed to create compositor backend`).
* `wayland-dev` carries the `libwayland-client` headers and `wayland-scanner` rawlink's
  `weston_capture_v1` capture needs. there is **no ffmpeg and no tigervnc**: the capture
  is in-process and wayland-native.
* `xkeyboard-config` provides the evdev keymap weston's fake-seat keyboard and the panel
  key taps use.
* `rust`/`cargo` build LIVI-Lite; the `-dev` packages above are what its build scripts
  link against (gstreamer-sys, wayland-sys, libudev, EGL/GLES via mesa-dev). `cmake`
  builds `aws-lc-sys` (the crypto half of the CarPlay stack), `perl` is used by its
  generated assembly.
* `sudo` is required at runtime: livi.service runs as `user`, and livi-core starts its
  helper (`livi-helperd`) as root through it. `install.sh` pre-seeds a validated
  `/etc/sudoers.d/99-LIVI-helper` rule for exactly that binary.
* the runtime GStreamer plugins cover LIVI's pipelines: `waylandsink` and the H.26x
  parsers (bad), `pulsesink`/`volume`/`aacparse`/RTP (good), `avdec_h264`/`avdec_h265`
  and the software fallback (libav), `faad` for CarPlay AAC.
* `systemd-dev` (or `eudev-dev` on a non-systemd image) provides `libudev.pc`, which
  `libudev-sys` needs; `gst-plugins-bad-dev` is only needed on releases that split it.
* tested with LIVI-Lite `main`, weston 16.0.0.

### 2. Build the host tools

    make -C ../ rawlink livi-cmd               # out/rawlink + out/livi-cmd (aarch64, musl)
    cc -O2 -Wall -Wextra -fPIC -DLIVI_WESTON_MAJOR=16 \
       $(pkg-config --cflags libweston-16 wayland-server) \
       -shared -o weston-touch.so ../weston-touch.c \
       $(pkg-config --libs libweston-16 wayland-server)

The repo Makefile picks the newest installed `libweston-*` and passes its major, because
weston 16 changed the touch and keyboard APIs (see `weston-touch.c`): building the weston
14 call against 16 **aborts weston on the first touch**, which drops the input socket.
`weston-touch.so` is the weston module that gives the compositor a touch device and
injects the panel keys rawlink sends (`k CODE` evdev taps); both are wayland events, so
there is no XTest mouse fallback to land on. `../` is the `rawplay/` directory.

### 3. LIVI-Lite from source

`install.sh` clones `https://github.com/pukikiko/LIVI-Lite` to `~/LIVI-Lite` when the
checkout is missing and builds the four workspaces (compositor, gst-host, helperd/core,
UI) natively with the phone's cargo. To use an existing checkout or pin a revision:

    ./install.sh --livi-lite ~/LIVI-Lite          # use this tree
    LIVI_LITE_REF=v9.3.0 ./install.sh             # pin a tag/commit
    LIVI_LITE_STAGE=/path/to/stage ./install.sh   # install a pre-built stage tree

By hand the build is one command; `livi-lite-build.sh` stages the installed layout
(`livi-core`, `livi-ui`, `resources/{driver,gst-host,compositor}` and the root templates):

    sh livi-lite-build.sh ~/LIVI-Lite /tmp/livi-stage
    sudo cp -a /tmp/livi-stage/. /opt/livi/

A native release build on a phone is the slow step (rustc, slint, aws-lc); a pre-built
`LIVI_LITE_STAGE` tree makes reinstalls and CI fast. A cross/CI build can drive the same
script with `CARGO_BUILD_TARGET`, `CARGO_TARGET_DIR` and `PKG_CONFIG_*` pointed at a
target sysroot.

### 4. Assets: the homebrew stick

`python3 ../../mkusb.py` builds `usb.img` with the launcher, hmi overlay and
`apps/rawplay`. This is the mass-storage image the gadget serves as `/fs/usb0`; on the
unit the launcher's carplay button runs `/fs/usb0/homebrew/apps/rawplay.sh`.

    sudo install -d /opt/livi
    sudo install -m 755 ../out/rawlink /opt/livi/rawlink
    sudo install -m 755 ../out/livi-cmd /opt/livi/livi-cmd
    sudo install -m 644 weston-touch.so /opt/livi/weston-touch.so
    sudo install -m 644 /path/to/usb.img /opt/livi/usb.img
    sudo install -m 755 livi-link-monitor /opt/livi/livi-link-monitor

### 5. LIVI configuration

The app reads `$HOME/.config/LIVI/config.json`; it must carry the panel size or the
nested compositor defaults to 1280x720 and weston's kiosk fullscreen rejects the window
(`xdg_surface geometry ... larger than the configured fullscreen state`):

    sudo install -m 644 livi-config.json /home/user/.config/LIVI/config.json
    sudo chown user:user /home/user/.config/LIVI/config.json

`dismissedPackages` in that file silences the "Missing Packages" dialog for the optional
Bluetooth/Wi-Fi/VA-API helpers the phone does not provide (its radios are driven by
LIVI's own helper + hostapd/BlueZ, not by the desktop stacks the list names).

On an install where LIVI has already run, `config.json` is the app's own full settings
file (carName, pairing, bindings, geometry), so `install.sh` does not replace it: it only
merges `wirelessAaEnabled: true` into it. The park is read when `livi` starts, so an
upgrade takes effect at the next livi restart or reboot; doing that over ssh drops the
connection when the AP takes wlan0. `install.sh` also seeds the `LIVI-vnc` config the old
Electron outer launcher used, so an upgrade from the AppImage keeps its old settings.

### 6. Units, power and the rawplay link state

    sudo install -m 644 livi-*.service rawlink.service /etc/systemd/system/
    sudo systemctl daemon-reload
    sudo systemctl enable livi-link livi-weston livi rawlink

* `livi-link.service` runs `livi-link-monitor`, the power state machine. It reads the
  charger from the power supply's `online` flag and the rawplay connection state from
  rawlink's own log, and moves the phone between three states (see "The rawplay link
  state" below). It also owns `livi.service`'s lifecycle: the unit is left enabled (so
  the app comes up at boot like before), but unplugged the monitor stops it and charging
  starts it again after the radios are confirmed. Wireless Android Auto is not part of
  the state machine: `livi-config.json` parks it on and LIVI owns it from there.
* **Suspend is not used.** `s2idle` is the only mode this kernel offers
  (`/sys/power/mem_sleep` is `[s2idle]`), and on this port it hangs in device suspend
  (the `wcn36xx` teardown logs `ERROR SMD_EVENT (312) not supported` and the DPU's
  vblank-waits never finish), no wake source fires - not the charger, not the power
  button - and the PMIC watchdog resets the phone minutes later. "Unplugged" is therefore
  deep CPU idle with the radios off, not suspend-to-RAM.
* The kernel wake lock (`/sys/power/wake_lock`) and `~/.cache/sxmo/sxmo.nosuspend` are
  held while `livi-link` runs, in **every** state, so nothing else can try that suspend.
  They only go away with the unit: `systemctl stop livi-link` releases both and unblocks
  the radios for maintenance.
* The latency hold is what keeps the link smooth. A wakelock does not stop the CPUs from
  entering deep idle, and with the phone's screen off the SoC sits in
  `cpu-power-collapse` (this A1 reports a 270 µs exit latency, ~83% residency idle)
  between wakeups. Every FunctionFS bulk-in completion and every PipeWire period then
  pays that latency: the raw link trickles (the panel draws frames line by line) and
  LIVI's audio stutters. While a player is connected the monitor holds the latency
  request at 0, keeping the CPUs shallow for the duration; idle (or unplugged) it drops
  it again, which is where most of the power saving comes from.
  `grep cpu-power-collapse /sys/devices/system/cpu/cpu0/cpuidle/state1/name` and watch
  its `usage` move again when the player exits.
* `install.sh` also retires an older install's `livi-wakelock.service` (the unconditional
  version of the wakelock), so an upgrade does not leave two units writing the same wake
  lock.
* If your install user is not `user`, change `User=`/`HOME=`/paths in the units (or let
  `install.sh` do it; `livi-link.service` passes the home through `LIVI_LINK_HOME`).

### 7. USB role

`rawlink.service` runs `rawlink-wait` as its main process. It forces the DWC3 role to
`device` (`/sys/class/usb_role/*/role`), because on this SoC the port sits at `host` until
something forces it; it then waits for `/sys/class/udc`, unbinds the initramfs NCM gadget
(`usb_gadget/g1`) and clears a stale functionfs mount, then `exec`s `rawlink`. The wait is
inside the main process, deliberately not an `ExecStartPre`: with an unplugged car a
synchronous `systemctl restart rawlink` would otherwise block in `start-pre` until the UDC
appeared (that is the "restart hangs" trap; the unit now starts in 0 s and binds the
moment device mode comes up). To use the port as a USB host instead (a wired CarPlay
dongle, say), stop `rawlink.service` and write `host` into the role switch. One port
cannot be both at once.

To pick up a changed `/opt/livi/usb.img`, just `sudo systemctl restart rawlink`; the old
mass-storage LUN stays on the old inode until the gadget rebinds. A restart with the car
attached can spend up to `TimeoutStopSec=10` in the stop if a bulk-in write is wedged, and
the unit's gadget re-enumerates afterwards (rawplay may need relaunching from the homebrew
menu).

### 8. The phone's own UI (fbkeyboard)

The phone's own screen is not part of the livi pipeline and runs fbkeyboard
(`postmarketos-ui-fbkeyboard`), the preferred UI for the appliance phone: the plain
framebuffer console with an on-screen keyboard for the touchscreen. It is a login console,
so ssh and a local shell both stay available.

If the image shipped a desktop (postmarketOS images are built around one UI), it has to be
retired - **after** fbkeyboard is installed, so the shared `postmarketos-base-ui` package
is never orphaned in between. `install.sh` retires xfce4/sxmo and the other desktop UI
metas; by hand it is:

    sudo apk del postmarketos-ui-xfce4 xfce4 xfce4-terminal xfce4-whiskermenu-plugin \
                 xfce4-pulseaudio-plugin onboard
    sudo apk del postmarketos-ui-sxmo-de-sway    # or -dwm/-i3/-river

The desktop display manager must not hold `tty1` (it would race the console), so `tinydm`
is disabled and the console getty plus the fbkeyboard unit are enabled:

    sudo systemctl disable --now tinydm
    sudo systemctl enable --now getty@tty1 fbkeyboard

`console-blank.service` is the UI's idle blanking: it runs
`setterm --blank 2 --powerdown 2` on `tty1`. On this panel the kernel's console blank
reaches the backlight - the `panel-otm1911` driver disables the WLED module, so
`/sys/class/backlight/backlight/bl_power` reads 4 - and any input unblanks again,
including fbkeyboard's injected key events (the same uinput path a touch takes). Change
the two minutes in the unit and `systemctl restart console-blank` to apply:

    sudo install -m 644 console-blank.service /etc/systemd/system/
    sudo systemctl daemon-reload
    sudo systemctl enable --now console-blank

`disable --now tinydm` ends a local desktop session, so run this over ssh if you are on
that desktop.

## The rawplay link state (charger, radios and CPU idle depth)

`livi-link-monitor` watches two things: the charger (the power supply's `online` flag)
and rawlink's own journal. Neither rawlink nor rawplay is changed or rebuilt for this —
the state is derived from lines rawlink already writes, and the monitor never talks to
LIVI's helper control socket. It does own LIVI's lifecycle through `systemctl`, though:
the app only runs while the radios are up.

| state | when | what happens |
|---|---|---|
| unplugged | the car's USB port is dead (no charger) | `livi.service` stopped, `rfkill block all`, bluetooth stopped and the radio drivers (`btqcomsmd`, `wcn36xx`) unloaded, `/dev/cpu_dma_latency` released (deep CPU idle into `cpu-power-collapse`) |
| idle | charging, no player | radio drivers reloaded, bluetooth started, `rfkill unblock all`, `livi.service` started, `/dev/cpu_dma_latency` released (deep CPU idle) |
| connected | charging, a rawplay player is on the link and reading | radios on, `livi.service` running, `/dev/cpu_dma_latency` pinned at 0 (shallow CPU idle, the 256 KB FunctionFS writes and PipeWire periods stay cheap) |

The charger is the car's USB port: on while the head unit is on. Unplugged overrides the
link state — if the car somehow runs without powering the port, the radios still go off,
LIVI still stops and the CPUs still deep-idle.

LIVI is part of the state machine: unplugging the charger (or, what is the same path,
killing the radios) stops `livi.service` with its staged stop, and charging starts it
again — but only after the radio reload and the `rfkill unblock` both succeeded, so a
half-up wcn36xx stack never brings the app back. The stop is re-checked every tick, not
only on the transition, so a `livi` that the multi-user boot job (or a manual
`systemctl start livi`) brings up behind the monitor is taken down again while the car
is off. `livi` still has `Restart=always`, so once it is up a crash is restarted by
systemd, and while charging the monitor keeps it owned: stopping it by hand brings it
back within a tick.

The radios are unloaded, not just rfkill-blocked: on this port the wcn36xx firmware
crashes on a plain `rfkill unblock` (the wcnss remoteproc takes a Data Abort and WLAN SMD
commands then time out, so wifi never returns). Unloading while soft-blocked and
reloading on the charger is the path that comes back cleanly.

The state file tracks the first column:

    cat /run/livi-link/state    # "unplugged <ts>", "idle <ts>" or "connected <ts>"

Wireless Android Auto deliberately stays out of this: toggling it under the running app
upsets LIVI's helper, so it is parked on in `livi-config.json` (`wirelessAaEnabled`) and
nobody touches it at runtime. `rfkill block all` takes wlan0 away with it; whether
hostapd comes back after `rfkill unblock all` depends on LIVI's helper, so check the
wireless section if the AP does not return.

The wake lock and `sxmo.nosuspend` are held in every state, because the only suspend this
port offers hangs and watchdog-resets the phone (see "Units" above); they keep sxmo or
anything else from trying it. Stopping `livi-link` releases the wakelock and unblocks the
radios for maintenance (and leaves `livi` as it is, so a `systemctl start livi` by hand
sticks while the monitor is stopped).

The lines the state machine matches (seen with `journalctl -u rawlink -o cat`):

* **connect** — `rawstream: reader ready at seq N` (rawplay sends `LI_READY` when it
  starts), an LI input forward (`usbgadget: unit touch/knob/msg`), or the 5 s stats line
  with a nonzero ack latency (`rawstream: ... latency 8.0/20.0 ms avg/max`; `0.0/0.0`
  means nobody is acking). The stats line is also how the monitor recovers "connected"
  after it is restarted mid-session, when no fresh `reader ready` will be written —
  `rawlink-wait` therefore keeps passing `--stats`.
* **idle** — `rawstream: no acks for 5 s, resending mode and resetting the window`
  (rawlink's own reader-silent watchdog, ~5 s after the player exits), a USB teardown
  line (`usbgadget: unit disabled the gadget`, `function unbound, will rebind`,
  `video write failed`, `ack/touch read failed`), a disconnected stream client
  (`rawstream: ... went away, reconnecting` / `waiting for ...`), or rawlink
  (re)starting or dying (`usbgadget: bound to ...`, systemd's `Started`/`Stopped` /
  `Main process exited` / `Scheduled restart` lines).

Inspect and debug:

    cat /run/livi-link/state                   # "unplugged", "idle" or "connected" <timestamp>
    journalctl -u livi-link -f                 # state transitions + the livi start/stop
    systemctl is-active livi                   # stopped while unplugged or radios unconfirmed
    journalctl -u rawlink -f                   # the lines it is matching on

`/etc/default/livi-link` overrides the monitor's knobs: `LIVI_LINK_IDLE_SECS` (how long
rawlink must be silent with the service inactive before idle), `LIVI_LINK_WAKE_REASSERT`
(how often the wake lock and sxmo flag are re-asserted), `LIVI_LINK_JOURNAL` (e.g. `-`
or `cat /tmp/rawlink.log` to drive it from something else while debugging),
`LIVI_LINK_POWER_SUPPLY` (default `qcom-smbchg-usb`), `LIVI_LINK_RFKILL` (default
`rfkill`, `LIVI_LINK_MODPROBE`, `LIVI_LINK_SYSTEMCTL`, `LIVI_LINK_RADIO_MODULES`). The
state machine and `livi-cmd` are covered by
`make -C rawplay livi-cmd && python3 rawplay/test_livi_link.py` on any host.

## Wireless Android Auto / CarPlay (the Rust helper)

LIVI-Lite's wireless paths run `resources/driver/livi-helperd` (the Rust rewrite of the
old python helper) as root through `sudo -n -E`: it talks to the host's **BlueZ**
(pairing agent, iAP2/AA Bluetooth profiles, discoverability) over the **system D-Bus**,
and it drives **hostapd** + **dnsmasq** to put an AP on the Wi-Fi interface. Nothing is
inside a chroot any more, so the helper sees the real `/run/dbus`, the real radios and
the real `systemctl`.

`install.sh` pre-seeds the one sudoers rule livi-core needs to start the helper
(`/etc/sudoers.d/99-LIVI-helper`, validated by `visudo`); from there the helper installs
its own udev rules, the `livi-wifi-ap` unit and the rest of the sudoers set on first run.
It also writes a `bluetooth.service` drop-in with `--noplugin=sap,midi`:
`sap` otherwise holds RFCOMM channel 8, which Android Auto's AAP wants, and `midi` takes a
128-bit EIR UUID slot CarPlay uses.

The helper only advertises (adapter `Alias` = `carName`, `Discoverable`/`Pairable`, AA
service) once its AP reports ready; if the AP is not ready it holds the Bluetooth
advertising back.

**The AP takes wlan0.** The wcn36xx firmware advertises no interface combinations, so the
phone cannot be a Wi-Fi client and an AP at once. Enabling wireless AA in LIVI's settings
drops the phone's own Wi-Fi (including ssh) for the duration of the session and puts
`10.10.0.1` on wlan0; disable it again in LIVI's UI to hand the interface back. With the
single USB port acting as the gadget to the unit, a second Wi-Fi adapter is not an option
while the car link is up.

Because toggling it under the running app upsets the helper, the deployment parks
`wirelessAaEnabled` **on** and leaves it alone: the AP and the BT advertising come up with
the app and stay up, and `livi-link` never talks to the helper's control socket (see "The
rawplay link state"). `install.sh` merges the key into LIVI's own settings file (never
replacing it), and the app reads it at start, so an ssh upgrade takes effect at the next
livi restart or reboot — the moment wlan0 goes to the AP and the connection drops. From
then on the phone's own Wi-Fi client and ssh are down for as long as the appliance runs;
the sections below assume a local shell.

LIVI's settings UI, its config file, or `livi-cmd` are how it comes down by hand. The
helper's control socket is `/tmp/cp-bt.sock` (the same path the app uses), so:

    sudo /opt/livi/livi-cmd /tmp/cp-bt.sock "set-aa 0"   # wlan0 back to NM
    sudo /opt/livi/livi-cmd /tmp/cp-bt.sock "set-aa 1"   # AP back up

A file park changes the boot state (the config is only read at `livi` start):

    sudo sed -i 's/"wirelessAaEnabled": *true/"wirelessAaEnabled": false/' \
        /home/user/.config/LIVI/config.json
    sudo systemctl restart livi

Check the helper:

    ps | grep livi-helperd                     # spawned by livi-core as root
    journalctl -u livi | grep '\[helper'       # startup state, profiles registered
    sudo bluetoothctl show | grep -E "Alias|Discoverable|Pairable"

With wireless AA up the unit appears to phones as `carName`.

## Audio

LIVI-Lite's GStreamer host uses the system `pulsesink`, so the app plays through the
phone's PipeWire by socket. `livi.service` points straight at the user's pulse socket -
there is no chroot bind any more:

    Environment=PULSE_SERVER=unix:/run/user/<uid>/pulse/native

Audio comes out of the phone's ordinary PipeWire sinks (speaker, headphones). Check it on
the host:

    pactl list short sinks

`/SystemVolume/` log lines show LIVI reading and setting the default sink volume. If
`pactl` is missing it logs `is pactl installed?` and audio stays silent.

Playback test (a wav through libpulse, then the app's exact AAC-LC chain with the system
gst):

    paplay /tmp/test.wav
    gst-launch-1.0 -q filesrc location=/tmp/test.aac ! aacparse ! faad \
      ! audioconvert ! audioresample ! pulsesink

### Who owns PipeWire (linger and the launcher race)

**The user manager must outlive a login.** The appliance has no desktop session any more
(fbkeyboard console, step 8), and `user@<uid>` only runs while somebody is logged in
unless lingering is enabled. `install.sh` runs `loginctl enable-linger user`, so the
enabled `pipewire.socket`/`pipewire-pulse.socket`/`wireplumber.service` come up at boot
with no session. Without it `/run/user/<uid>/pulse/native` does not exist when `livi`
starts, `livi-chroot`'s bind fails after its 30 s wait, and the app is mute for that whole
run - logging in on tty1 later does not repair it, because the bind only happens at livi
start. Check with `loginctl show-user user -p Linger` (must be `yes`) and
`systemctl is-active user@10000`.

postmarketOS desktop sessions autostart `/usr/libexec/pipewire-launcher` (via
`/etc/xdg/autostart/pipewire.desktop`), and the systemd user socket units
`pipewire.socket`/`pipewire-pulse.socket` are enabled too. Both start at login and race
for `/run/user/<uid>/pipewire-0`: the launcher pkills the existing stack, takes the
pipewire lock and rebinds the socket path while systemd's socket unit still holds it, so
the socket-activated `pipewire.service` fails to lock (`unable to lock lockfile
'/run/user/<uid>/pipewire-0.lock' ... maybe another daemon is running`), restart-loops
into `start-limit-hit`, and the path can be left attached to a dead listener. Every
client then gets `Connection refused` on `pipewire-0` and `pulse/native` (`ss` shows no
listener even though stale socket files exist), and LIVI is silent while its UI works
perfectly.

`install.sh` hides the autostart (the `pipewire.desktop` override goes to
`~/.config/autostart`), so the systemd user units are the only owner: they are supervised
(`Restart=on-failure`) and socket-activate pulse on the first client. `livi.service` points
PULSE_SERVER straight at that socket, so a livi started before the user manager comes up
mute for that run: start it after boot with `systemctl restart livi`. To repair an install
that already lost the race (or to restart audio by hand):

    export XDG_RUNTIME_DIR=/run/user/10000
    systemctl --user stop pipewire-pulse.socket pipewire.socket \
        wireplumber.service pipewire-pulse.service pipewire.service
    pkill -u user -fx /usr/bin/pipewire-pulse
    pkill -u user -fx /usr/bin/wireplumber
    pkill -u user -fx /usr/bin/pipewire
    rm -f $XDG_RUNTIME_DIR/pipewire-0 $XDG_RUNTIME_DIR/pipewire-0-manager \
          $XDG_RUNTIME_DIR/pipewire-0.lock $XDG_RUNTIME_DIR/pipewire-0-manager.lock \
          $XDG_RUNTIME_DIR/pulse/native $XDG_RUNTIME_DIR/pulse/pid
    systemctl --user reset-failed
    systemctl --user enable --now pipewire.socket pipewire-pulse.socket wireplumber.service

Afterwards `pactl info` must report a default sink and `systemctl --user status
pipewire.service` must say `active (running)`, not `failed (start-limit-hit)`.

## Boot behaviour

All four units are `WantedBy=multi-user.target` with `Restart=always` and
`StartLimitIntervalSec=0` (restart forever, no rate limit). With the charger present
(and after the radios are confirmed) all four run; unplugged, `livi` is the one the
state machine takes down:

    $ systemctl is-active livi-link livi-weston livi rawlink
    active active active active

The phone UI is separate from that chain: `getty@tty1`, `fbkeyboard` and `console-blank`
come up with multi-user too, and `tinydm` is disabled so nothing else takes `tty1`:

    $ systemctl is-active getty@tty1 fbkeyboard console-blank
    active active active

* `livi-link`, `livi-weston`, `livi` and `rawlink` all enter within a
  few seconds of multi-user; weston creates `wayland-livi` in `/run/livi`, the gadget
  binds as soon as rawlink starts (and its capture thread reconnects to weston on its
  own), `livi-link` comes up `unplugged` or `idle` depending on the charger and starts
  watching the journal, and LIVI brings wireless Android Auto up from its config (wlan0
  goes to the AP; see the wireless section). On an unplugged boot the multi-user `livi`
  still starts, but `livi-link` stops it on its next tick (the state file stays
  `unplugged`; the app comes back when the charger does).
* each program has its own unit, so a crash restarts only that program; killing weston or
  the app brings the whole chain back through the dependencies.
* LIVI-Lite is one process tree under `livi.service`: livi-core starts the nested
  compositor, the Slint UI, the GStreamer host and the helper itself. Stopping is
  graceful by design: the unit runs `KillMode=mixed`, so only livi-core gets the SIGTERM;
  its signal handler stops the UI first, then the compositor (so the UI never sees its
  display vanish), then the helper. `TimeoutStopSec=8` SIGKILLs whatever is left, which
  keeps `systemctl restart livi` bounded even when a decoder wedges. The native helper
  means there is no python/asyncio layer and no `LIVI_STOP_GRACE` supervisor loop any
  more.

Verification after a reboot:

    systemctl is-active livi-link livi-weston livi rawlink
    cat /run/livi-link/state                                # idle <timestamp> (or unplugged/connected)
    systemctl is-active livi                                # stopped (inactive) while unplugged
    cat /sys/power/wake_lock                                # livi
    rfkill list                                             # blocked while unplugged
    sudo cat /sys/kernel/config/usb_gadget/liviraw/UDC      # 7000000.usb
    cat /sys/class/udc/7000000.usb/state                    # not attached / configured
    sudo journalctl -u rawlink -n 5                         # bound to ..., wayland capture on ...
    systemctl is-active getty@tty1 fbkeyboard console-blank # the phone ui
    XDG_RUNTIME_DIR=/run/livi WAYLAND_DISPLAY=wayland-livi weston-screenshooter

Timing reference (measured on the aarch64 Buildroot qemu image, which runs
the same LIVI-Lite binaries and units): the appliance is at `multi-user` in
~11 s and the panel gets LIVI-Lite ~12 s after weston is up; the previous
Electron deployment took ~102 s from weston to a presented UI on the same
image. A native x86_64 build starts core to Slint UI in ~0.6 s, so on phone
hardware the app layer is no longer the long pole - the radio ~reload and
the charger checks in `livi-link-monitor` are.

## Manual runs and `livi.sh`

The units do not call `livi.sh` (each program restarts separately), but the stock script
still works on the phone against the installed LIVI-Lite:

    ./livi.sh start          # uses /opt/livi/livi-core + /opt/livi/resources
    ./livi.sh stop

To run a source build instead, point it at the cargo output and the checkout:

    LIVI_CORE=~/LIVI-Lite/native/livi-helperd/target/release/livi-core \
      LIVI_ROOT=~/LIVI-Lite ./livi.sh start

While the units run, `livi.sh start` sees the weston socket and the running `livi-core`
and starts nothing.

## Troubleshooting

| symptom | cause / fix |
|---|---|
| `rawstream: wayland ... not up (No such file or directory), retrying` forever, 0 frames | rawlink started before weston; the capture thread retries every second and recovers when `livi-weston` is up. If it never comes up check `systemctl status livi-weston` and its journal |
| `rawstream: wayland capture failed: unauthorized` | weston refuses every `weston_capture_v1` shot unless an authority allows it: `livi-weston.service` must run weston with `--debug` (its allow-all screenshot authority). an old unit without it never captures; re-run `install.sh` |
| `rawstream: weston output is 1280x720, expected 800x480` | weston came up with the wrong mode; the unit pins `--width 800 --height 480` (a local `weston.ini` can override the headless output) |
| panel black but frames flow; `~/.config/LIVI/log/compositor.log` has `eglCreateImageKHR createImageFromDmaBufs failed` / `create_immed failed and produced an invalid wl_buffer` | the nested compositor's EGL cannot import the decoder's dmabufs. On a pixman session that means the codec probe picked a hardware `v4l2*dec`/`va*dec`; make the plugin set match the phone (software `avdec_*`/`faad` are enough) or run a GL session (`LIVI_WESTON_RENDERER=gl`, `--renderer=gl`) on a machine with a working GPU |
| picture is upside down on a GL session | weston's async GL capture is bottom-up on drivers without `GL_ANGLE_pack_reverse_row_order` (NVIDIA); start rawlink with `--flip`. pixman sessions are never affected |
| frames draw line by line and/or audio stutters while the phone's screen is off | the CPUs sit in `cpu-power-collapse` between wakeups, taxing every FunctionFS completion and PipeWire period. `livi-link` must be in `connected` and holding `/dev/cpu_dma_latency` at 0 while a player is connected: check `cat /run/livi-link/state` and `journalctl -u livi-link` (it releases the latency by design in `idle` and `unplugged`); reinstall the current `livi-link-monitor`/unit and `rawlink` (1 MB video pipe, 256 KB ep1 writes); see the host section of `rawplay/README.md` |
| `weston ... failed to create compositor backend` | `weston-backend-headless` (and/or `weston-shell-kiosk`) is not installed; `apk add weston-backend-headless weston-shell-kiosk` |
| `livi-compositor` exits right after `new output` | stale sockets from a hard kill: remove `$XDG_RUNTIME_DIR/livi/compositor.ctrl` (and `.lock`) and restart `livi` |
| `xdg_surface geometry (1280x720) is larger than ... (800x480)` | `~/.config/LIVI/config.json` missing or not 800x480 (the inner app reads `$HOME/.config/LIVI`, not `--user-data-dir`) |
| `usbgadget: no usb device controller` looping | phone not in device mode; `rawlink-wait` forces the role and waits for `/sys/class/udc` |
| `systemctl restart rawlink` hangs | old unit had the wait in `ExecStartPre`; it is now in `rawlink-wait` (the main process), so start/restart return immediately even unplugged |
| `systemctl restart livi` hangs in `stop-sigterm` | an old unit from the Electron deployment is still installed. Re-run `install.sh` (the current `livi.service` + native core stop bounded by `TimeoutStopSec=8`); check `systemctl cat livi` shows `ExecStart=/opt/livi/livi-core` and `KillMode=mixed` |
| changed `usb.img` not visible to the unit | the LUN holds the old file until the gadget rebinds: `sudo systemctl restart rawlink` |
| LIVI shows no Bluetooth device | the Rust helper is not running: `journalctl -u livi \| grep '\[helper'`; the usual cause is the sudoers rule (`/etc/sudoers.d/99-LIVI-helper`) missing or not matching `/opt/livi/resources/driver/livi-helperd`, or `bluetooth.service` not being up. Re-run `install.sh`, then `systemctl restart livi` |
| `[core] cannot start the helper` / `sudo: a password is required` | the pre-seeded sudoers rule is missing. Re-run `install.sh` (it writes and `visudo`-validates it), or add `user ALL=(root) NOPASSWD: SETENV: /opt/livi/resources/driver/livi-helperd` to `/etc/sudoers.d/99-LIVI-helper` |
| wireless session connects but the screen stays black, `decoder=v4l2h265dec` | the hardware path needs dmabuf and the pixman compositor has none. Get the software path back by making the probe report `hw=false sw=true` (the appliance's plugin set is `gst-libav` + `gst-plugins-bad`; clear `~/.cache/gstreamer-1.0` after changing plugins) |
| black screen, codec probe `h264(hw=false sw=false)` | the decoder/waylandsink plugins are not installed. `apk add gst-plugins-bad gst-libav` and clear `~/.cache/gstreamer-1.0` |
| video sheared/smeared while the UI is perfect | no longer expected with LIVI-Lite: the video host links the system GStreamer, whose waylandsink and videoconvert agree on the row stride. If it appears, the system GStreamer is mixed-version (update `gst-plugins-base`/`gst-plugins-bad` together) |
| no audio from LIVI; `pactl info` in the chroot says `Connection refused`; `systemctl --user status pipewire.service` is `failed (start-limit-hit)` | the desktop `pipewire-launcher` autostart raced the systemd user units at login and left `pipewire-0`/`pulse/native` pointing at a dead listener. Re-run `install.sh` (it hides the launcher and repairs the stack) or run the repair block under "Who owns PipeWire" |
| no audio from LIVI after the fbkeyboard switch; `[SystemVolume] could not set @DEFAULT_SINK@ ... is pactl installed?` | nothing starts the user manager at boot without a login session any more, so the pulse socket is not there when `livi` starts. `install.sh` runs `loginctl enable-linger user`; by hand: `sudo loginctl enable-linger user && sudo systemctl start user@10000`, then `systemctl restart livi`. Check `loginctl show-user user -p Linger` (`yes`) and that `/run/user/10000/pulse/native` exists |
| no audio from LIVI, and `paplay` hangs on the host too | `pipewire-pulse` is wedged or gone (`journalctl _UID=10000 \| grep -E 'create_stream_timeout\|pipewire'`). Restart the user audio stack per "Who owns PipeWire" (with `XDG_RUNTIME_DIR=/run/user/10000`) |
| no audio / `is pactl installed?` | install `pulseaudio-utils`; make sure `livi.service` has `PULSE_SERVER=unix:/run/user/<uid>/pulse/native` |
| no Wi-Fi AP / phone never projects | `journalctl -u livi \| grep wifi_ap`; the helper runs hostapd directly, check `logread`/`dmesg` for wlan0 and the helper's sudoers rule |
| phone's Wi-Fi/ssh is down (or was never reachable) while the appliance runs | expected on wcn36xx (no STA+AP concurrency) and by design: wireless AA is parked on in `livi-config.json` so it is never toggled under the running app. Toggle it off in LIVI's UI, or from a local shell with `/opt/livi/livi-cmd /tmp/cp-bt.sock "set-aa 0"`, to get wlan0 back. Also expected in the `unplugged` state: `livi-link` stops `livi` and rfkill-blocks the radios while the charger is gone; plug the charger in (or `systemctl stop livi-link`) to get them back |
| LIVI is not on the panel although the phone is up | either the charger is gone (`cat /run/livi-link/state` says `unplugged`) or the radio reload failed after it returned (`journalctl -u livi-link` shows `radios not confirmed up`), in which case `livi` stays stopped by design. Plug/unplug once or `systemctl restart livi-link` to retry; check `rfkill list` and `dmesg` for the wcn36xx Data Abort row above |
| wireless AA never comes up | it is LIVI's own now, not `livi-link`'s: check `journalctl -u livi \| grep '\[helper'` (the helper must run) and the AP side (`journalctl -u livi \| grep wifi_ap`). `wirelessAaEnabled` must be `true` in `livi-config.json`; re-run `install.sh` if it is missing |
| no `/fs/usb0` on the unit | the port must be high speed (jailbroken `io-usb` restart, `usb/homebrew/apps/usbhs.sh`); check `--stick /opt/livi/usb.img` exists |
| phone unreachable after a while | either the charger is gone (`livi-link` turns the radios off by design: `cat /run/livi-link/state`, `rfkill list`, plug the charger in) or `livi-link` is not running (`systemctl is-active livi-link`; only `systemctl stop livi-link` releases the wake lock and restores the radios) |
| wifi/AP does not come back after the charger returns, `dmesg` shows `qcom-wcnss-pil ... Data Abort` | a bare `rfkill unblock` crashes the wcn36xx firmware on this port. `livi-link` unloads `btqcomsmd`/`wcn36xx` while blocked and reloads them on the charger; if it was toggled by hand, `sudo modprobe -r btqcomsmd wcn36xx && sudo modprobe wcn36xx btqcomsmd && sudo systemctl restart bluetooth` (or reboot) |
| `echo mem`/`systemctl suspend` resets the phone | expected on this port: s2idle hangs in device suspend with no working wake source and the PMIC watchdog resets the phone minutes later. `livi-link` holds the wake lock so nothing else tries it; do not stop the unit and suspend by hand |
| `livi-core: not found` / `cannot execute` | a stale unit from the Electron deployment (`ExecStart=/opt/livi/livi-chroot`). Re-run `install.sh`; `systemctl cat livi` must show `/opt/livi/livi-core` |
| `livi-core` starts but the UI never appears | check `journalctl -u livi \| grep -E 'UI started\|compositor\|no runtime'`; `LIVI_RESOURCES` must point at `/opt/livi/resources` and the four binaries must be staged there (`resources/driver`, `resources/gst-host`, `resources/compositor`, `livi-ui`) |
| no touch, and panel keys do nothing, while the video works | the `weston-touch` module did not load, or weston restarted: check `journalctl -u livi-weston` for `livi-touch: touch device ready on ...` and for a module load error. building `weston-touch.so` without `-DLIVI_WESTON_MAJOR=16` on weston 16 aborts weston on the first touch. rawlink retries the socket every 2 s, so it also recovers from a weston restart |
| no touch at all after a reboot, while the car link works | rawlink won the boot race before weston existed; an old `/opt/livi/rawlink` disabled injection at startup. the current build keeps injection enabled and retries the weston-touch socket on use; update it (`make -C rawplay rawlink` on the phone or re-run `install.sh`) and `systemctl restart rawlink` |
| weston core-dumps on restart | weston 16 asserts the touch device list is empty at shutdown; `weston-touch.c` destroys its device in the compositor destroy listener |
| phone screen never blanks / stays lit | the phone UI's idle blanking is not applied: `systemctl is-active console-blank`, then `systemctl restart console-blank` puts `setterm --blank 2 --powerdown 2` on `tty1` (edit the unit's interval to change it). a desktop's idle handling never blanked this panel; the console path is what reaches the WLED backlight |
| fbkeyboard missing / no on-screen keys | the unit or its uinput module is down: `systemctl status fbkeyboard`, `ls -l /dev/uinput`, `journalctl -u fbkeyboard`; reinstall `postmarketos-ui-fbkeyboard` (step 8) if the unit is gone. a desktop UI still holding `tty1` (`systemctl is-active tinydm`) draws over the console too |

## Limits

* **Idle is deep CPU idle, not suspend.** s2idle on this port hangs in device suspend
  (wcn36xx/DPU) with no working wake source and watchdog-resets the phone minutes later,
  so `livi-link` keeps the wakelock in every state and switches the radios off with
  rfkill instead. The phone is intentionally off the air while unplugged; plug the
  charger in to bring the radios back.
* **Wireless AA owns wlan0 for the whole appliance run** (no STA+AP concurrency on
  wcn36xx): it is parked on in `livi-config.json` so it is never toggled under the
  running app, which means the phone's own WiFi and ssh are down until it is turned off
  in LIVI's UI (or with `livi-cmd`).
* **No wired CarPlay dongle on the same port.** The single USB port is the gadget to the
  unit; a Carlinkit needs host mode. Wireless (LIVI Link network dongle) is the way to
  have both.
* The stack is CPU-rendered (weston pixman + the compositor's software EGL): fine for
  800x480 UI, not a GPU pipeline.
* The UI still lists the optional Wi-Fi/BT/audio helper packages as dismissed: the
  phone's radios are driven by LIVI's own helper + hostapd/BlueZ, not by the desktop
  stacks (NetworkManager, PipeWire's bluetooth plugin) that list names.
