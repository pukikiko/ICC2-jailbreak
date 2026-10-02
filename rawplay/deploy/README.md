# LIVI + rawlink on a postmarketOS phone (the appliance build)

This is the deployment that turns a postmarketOS phone into the head-unit side of the raw
livi link: the phone runs the **unmodified aarch64 LIVI release** and `rawlink`, its USB
port enumerates as the car's USB gadget (vendor video interface + the homebrew stick), and
the unit's 800x480 panel becomes LIVI's display with touch/knob/button coming back up the
same wire. Everything is a systemd unit that starts at boot and restarts on its own. One
of them, `livi-link`, reads rawlink's own log and follows the rawplay connection state: a
connected player gets the shallow-CPU-idle wakelock, an idle link releases it so the SoC
deep-idles (see "The rawplay link state"). Wireless Android Auto is parked on in LIVI's
config and left alone: toggling it under the running app upsets LIVI's helper.

Reference hardware: **Xiaomi Mi A1 (qcom-msm8953, "tissot")**, postmarketOS edge with
systemd. The virtual display is 800x480 to match the unit's panel; the phone's own screen
is not part of the pipeline. The stack itself is generic: any aarch64 postmarketOS phone
with a configfs/functionfs-capable UDC works.

```
 phone (postmarketOS, musl)                          car (i.MX31, QNX, usb host)
 ┌──────────────────────────────────────────┐        ┌──────────────────────────┐
 │ LIVI AppImage (glibc)  ── ubuntu chroot  │        │                          │
 │      │ wayland                           │        │                          │
 │ weston (kiosk, pixman) ── Xvnc :9 800x480│        │                          │
 │      │ x11grab                           │        │                          │
 │ rawlink stream ── functionfs vendor bulk ├───────►│ rawplay → IPU scanout    │
 │ rawlink gadget ── mass_storage (usb.img) │◄───────┤ /fs/usb0/homebrew/apps   │
 │      ▲ touch/knob/button (LI messages)   │        │ touch/knob/buttons       │
 └──────┴───────────────────────────────────┘        └──────────────────────────┘
        USB-C (device)               USB-A (host)
```

## Why a chroot

LIVI's release builds are glibc Electron bundles; postmarketOS is musl. `gcompat` cannot
load Electron (`unsupported relocation type 1032`), so the AppImage runs inside a small
**Ubuntu base rootfs** (aarch64 glibc) with the host's X11/Wayland/audio sockets
bind-mounted in. `livi-chroot` is the launcher/supervisor that does the mounts and runs the
AppImage; it also stays alive while LIVI's detached nested compositor lives, because the
AppImage launcher exits by design (`src/main/index.ts`: "outer launcher hands off to the
nested compositor and exits") and a plain `exec` would leave systemd with a finished unit
whose cgroup takes the real app down.

**Use Ubuntu 26.04 (or Debian Trixie), not 24.04.** The release's bundled GStreamer wants
newer host libraries than 24.04 has: `libavcodec` links `libva.so.2` for `vaMapBuffer2`
(libva >= 2.21) and `libgstwaylandsink` links `wl_display_create_queue_with_name`
(libwayland >= 1.23). On 24.04 those plugins silently fail to load, the codec probe
reports `h264(hw=false sw=false)`, and everything wireless projects into a black screen
while otherwise working perfectly. On 26.04 the probe reports `h264(hw=true sw=true)`.

## Files here

| file | what it is |
|---|---|
| `install.sh` | automated deployment on the phone (packages, build, chroot, units) |
| `livi-chroot` | the chroot launcher/supervisor (also stages the stop, see below); goes to `/opt/livi/livi-chroot` |
| `livi_asyncio_compat.py`, `livi-asyncio-compat.pth` | python 3.14 fix for the wireless helper (see below) |
| `livistride.c` | preload shim that fixes the video plane's wl_shm stride (see below) |
| `livi-config.json` | LIVI settings: 800x480 kiosk, wireless AA parked on, and the optional-package dialog dismissed |
| `livi-link-monitor` | the rawplay-link power state machine: watches rawlink's log and holds the CPU idle depth (see "The rawplay link state") |
| `livi-cmd.c` | one-line client for LIVI's helper control socket (`set-aa`), for toggling wireless AA by hand; goes to `/opt/livi/livi-cmd` |
| `livi-link.service` | runs `livi-link-monitor` as root at boot, restart-forever |
| `pipewire.desktop` | xdg autostart override that hides the desktop's pipewire-launcher so the systemd user units own audio (see "Audio") |
| `livi-xvnc.service` | TigerVNC X server `:9` at 800x480 (the screen everything draws into) |
| `livi-weston.service` | weston x11/kiosk/pixman on `:9`; hosts the app and the video plane |
| `livi.service` | the LIVI AppImage, through `livi-chroot` |
| `rawlink.service` | `rawlink run`: functionfs gadget + mass storage + `:9` sender |
| `rawlink-wait` | forces device mode, waits for the UDC, then execs rawlink (the unit's main process) |

## Deploy, step by step

Run as a user with sudo on the phone. `install.sh` does all of this; the steps are here
for when something needs doing by hand.

### 1. Packages

     sudo apk add build-base bash ffmpeg weston weston-dev weston-backend-x11 \
                  weston-shell-kiosk tigervnc fuse3 xkeyboard-config

* `ffmpeg` is rawlink's capture source (`x11grab`). rawlink execs it off `PATH`; without
  it the unit logs `rawstream: capture pipe closed, restarting ffmpeg` forever and never
  draws a frame.

* `weston-backend-x11` and `weston-shell-kiosk` are separate subpackages; without them
  weston fails with `failed to create compositor backend`.
* `bash` is needed by the AppImage's `AppRun`; postmarketOS has busybox `ash` only.
* tested with LIVI 8.3.0, weston 16.0.0, tigervnc 1.16.2, Ubuntu base 24.04.3.

### 2. Build the host tools

    make -C ../ rawlink livi-cmd               # out/rawlink + out/livi-cmd (aarch64, musl)
    cc -O2 -Wall -Wextra -fPIC -DLIVI_WESTON_MAJOR=16 \
       $(pkg-config --cflags libweston-16 wayland-server) \
       -shared -o weston-touch.so ../weston-touch.c \
       $(pkg-config --libs libweston-16 wayland-server)

The repo Makefile picks the newest installed `libweston-*` and passes its major, because
weston 16 changed the touch API (see `weston-touch.c`): building the weston 14 call
against 16 **aborts weston on the first touch**, which drops the touch socket and leaves
rawlink on its XTest mouse fallback (pointer drags, a cursor, taps in the wrong place).
`weston-touch.so` is the weston module that gives the compositor a touch device rawlink
can drive, so the app gets real touch instead of an X11 mouse. `../` is the `rawplay/`
(aka `rawplay/`) directory.

### 3. Assets

* **LIVI** — the standard aarch64 release AppImage, e.g.
  `https://github.com/f-io/LIVI/releases/download/v8.3.0/LIVI-8.3.0-linux-arm64.AppImage`.
  No patches; `livi.sh`'s environment is the supported one.
* **The homebrew stick** — `python3 ../../mkusb.py` builds `usb.img` with the
  launcher, hmi overlay and `apps/rawplay`. This is the mass-storage image the gadget
  serves as `/fs/usb0`; on the unit the launcher's carplay button runs
  `/fs/usb0/homebrew/apps/rawplay.sh`.

Install both under `/opt/livi`:

    sudo install -d /opt/livi
    sudo install -m 755 ../out/rawlink /opt/livi/rawlink
    sudo install -m 755 ../out/livi-cmd /opt/livi/livi-cmd
    sudo install -m 644 weston-touch.so /opt/livi/weston-touch.so
    sudo install -m 644 /path/to/usb.img /opt/livi/usb.img
    sudo install -m 755 livi-chroot /opt/livi/livi-chroot
    sudo install -m 755 livi-link-monitor /opt/livi/livi-link-monitor

### 4. The glibc chroot

    sudo mkdir -p /opt/livi/rootfs
    curl -fL https://cdimage.ubuntu.com/ubuntu-base/releases/26.04/release/ubuntu-base-26.04.1-base-arm64.tar.gz \
      | sudo tar -xz -C /opt/livi/rootfs
    sudo mount --bind /dev /opt/livi/rootfs/dev
    sudo mount -t proc proc /opt/livi/rootfs/proc
    sudo mount -t sysfs sys /opt/livi/rootfs/sys
    sudo cp /etc/resolv.conf /opt/livi/rootfs/etc/resolv.conf
    sudo cp LIVI-8.3.0-linux-arm64.AppImage /opt/livi/rootfs/opt/LIVI.AppImage
    sudo chroot /opt/livi/rootfs /bin/bash -c \
      'apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
         libgtk-3-0t64 libnss3 libxss1 libxtst6 libgbm1 libasound2t64 libatspi2.0-0t64 \
         libsecret-1-0 libnotify4 libcups2t64 libdbus-1-3 libexpat1 libfontconfig1 \
         fonts-dejavu-core libxkbcommon0 libxkbcommon-x11-0 libxrandr2 libxcomposite1 \
         libxdamage1 libxfixes3 libxext6 libx11-xcb1 libxcb-dri3-0 libxcb-xkb1 libxcb-shm0 \
         libxcb-randr0 libxcb-render0 libxcb-sync1 libxcb-xfixes0 libxcb-shape0 libxcb-glx0 \
         libgl1 libegl1 libgles2 libglx-mesa0 libgl1-mesa-dri libpango-1.0-0 libcairo2 \
         libgdk-pixbuf-2.0-0 libatk1.0-0t64 libatk-bridge2.0-0t64 libfuse2t64 libfuse3-4 \
         libxshmfence1 libdrm2 libssh-4 libgudev-1.0-0 \
         libva2 libva-drm2 libva-x11-2 libva-wayland2 libpulse0 pulseaudio-utils \
         python3 python3-dbus python3-gi gir1.2-glib-2.0 python3-smbus2 python3-pip \
         python3-yaml gcc libc6-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
         bluez iproute2 iw rfkill hostapd dnsmasq-base procps sudo network-manager'

`libssh-4` lets the bundled ffmpeg plugin load at all; `libva2` (>= 2.21) is needed for
the bundled H.264 decoder, `libgudev-1.0-0` for the remaining v4l2 helpers. `install.sh`
also **removes** `libv4l-0t64` again: with it the bundled stateful v4l2 plugin loads, the
codec probe reports `hw=true`, and the app builds a `v4l2h26xdec` pipeline whose dmabuf
output the pixman nested compositor cannot take (`waylandsink` logs `Could not bind to
zwp_linux_dmabuf_v1`, the pipeline dies `not-negotiated`, the screen stays black). Without
it the probe reports `hw=false sw=true` and the bundled `avdec_h26x` software decoders
feed waylandsink through `wl_shm`, which pixman composites fine.

Then the two in-chroot extras `install.sh` also does:

    # python 3.14 shim for the wireless helper
    sudo install -m 644 livi_asyncio_compat.py /opt/livi/rootfs/usr/lib/python3/dist-packages/
    sudo install -m 644 livi-asyncio-compat.pth /opt/livi/rootfs/usr/lib/python3/dist-packages/
    # the waylandsink stride shim; gcc and the gstreamer headers came with the packages
    # above and match the bundled 1.28 ABI
    sudo mkdir -p /opt/livi/rootfs/opt/livi
    sudo install -m 644 livistride.c /opt/livi/rootfs/tmp/livistride.c
    sudo chroot /opt/livi/rootfs /bin/bash -c \
      'gcc -O2 -fPIC -shared -I/usr/include/gstreamer-1.0 -I/usr/include/glib-2.0 \
       -I/usr/lib/aarch64-linux-gnu/glib-2.0/include \
       -o /opt/livi/livi-stride-fix.so /tmp/livistride.c'

`livi-chroot` re-does the `/dev`, `/proc`, `/sys`, `$XDG_RUNTIME_DIR`, `/run/dbus`,
the pulse socket/cookie and `/tmp/.X11-unix` mounts itself (idempotently), so the manual
mounts above are only needed for `apt` during setup.

### 5. LIVI configuration

The outer launcher reads `--user-data-dir`'s `config.json`, but the real (inner) app uses
`$HOME/.config/LIVI/config.json`. Both must carry the panel size or the nested compositor
defaults to 1280x720 and weston's kiosk fullscreen rejects the window
(`xdg_surface geometry ... larger than the configured fullscreen state`):

    sudo install -m 644 livi-config.json /opt/livi/rootfs/home/user/.config/LIVI/config.json
    sudo install -m 644 livi-config.json /opt/livi/rootfs/home/user/.config/LIVI-vnc/config.json
    sudo chown 1000:1000 /opt/livi/rootfs/home/user/.config/LIVI/config.json \
                         /opt/livi/rootfs/home/user/.config/LIVI-vnc/config.json

`dismissedPackages` in that file silences the "Missing Packages" dialog for the optional
Bluetooth/Wi-Fi/VA-API helpers that a postmarketOS chroot cannot usefully provide.

On an install where LIVI has already run, `config.json` is the app's own full settings
file (carName, pairing, bindings, geometry), so `install.sh` does not replace it: it only
merges `wirelessAaEnabled: true` into it. The manual `install` commands above are for a
fresh rootfs. The park is read when `livi` starts, so an upgrade takes effect at the next
livi restart or reboot; doing that over ssh drops the connection when the AP takes wlan0.

### 6. Units, suspend and the rawplay link state

    sudo install -m 644 livi-*.service rawlink.service /etc/systemd/system/
    sudo systemctl daemon-reload
    sudo systemctl enable livi-link livi-xvnc livi-weston livi rawlink

* `livi-link.service` runs `livi-link-monitor`, which derives the rawplay connection
  state from rawlink's own log and moves the phone between two levels: with a player on
  the link it pins `/dev/cpu_dma_latency` at 0, with no player it releases it so the SoC
  deep-idles. The full mechanism, the log lines it keys on and its limits are under
  "The rawplay link state" below. Wireless Android Auto is not part of the state
  machine: `livi-config.json` parks it on and LIVI owns it from there.
* The kernel wake lock (`/sys/power/wake_lock`) and `~/.cache/sxmo/sxmo.nosuspend` are
  held while `livi-link` runs, in **both** states. Without them Sxmo suspends the idle
  phone (`rtcwake -m mem`), WiFi and the gadget go away, and the car screen is dead until
  the power button is pressed. They are what keeps the appliance connectable, so they
  only go away with the unit: `systemctl stop livi-link` releases both and hands the
  phone back to sxmo.
* The latency hold is what keeps the link smooth. A wakelock does not stop the CPUs from
  entering deep idle, and with the phone's screen off the SoC sits in
  `cpu-power-collapse` (this A1 reports a 270 µs exit latency, ~83% residency idle)
  between wakeups. Every FunctionFS bulk-in completion and every PipeWire period then
  pays that latency: the raw link trickles (the panel draws frames line by line) and
  LIVI's audio stutters. While a player is connected the monitor holds the latency
  request at 0, keeping the CPUs shallow for the duration; idle it drops it again, which
  is where most of the idle power saving comes from.
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

## The rawplay link state (CPU idle depth)

The phone is the appliance only while somebody is watching: `livi-link-monitor` reads
rawlink's own journal and moves the CPU idle depth between two levels to match. Neither
rawlink nor rawplay is changed or rebuilt for this — the state is derived from lines
rawlink already writes, and the monitor never talks to LIVI at all.

| state | when | what happens |
|---|---|---|
| connected | a rawplay player is on the link and reading | `/dev/cpu_dma_latency` pinned at 0 (shallow CPU idle, the 256 KB FunctionFS writes and PipeWire periods stay cheap) |
| idle | no player | `/dev/cpu_dma_latency` released (the SoC deep-idles into `cpu-power-collapse`) |

Wireless Android Auto deliberately stays out of this: toggling it under the running app
upsets LIVI's helper, so it is parked on in `livi-config.json` (`wirelessAaEnabled`) and
nobody touches it at runtime. The cost is that wlan0 stays on LIVI's AP for as long as
the appliance runs (no STA+AP concurrency on wcn36xx), so the phone's own Wi-Fi and ssh
are down; see the wireless section for bringing them back by hand.

The wake lock and `sxmo.nosuspend` are held in both states: a suspend takes the USB
gadget and WiFi down with it, and a suspended phone can never see a later rawplay client.
So "idle" is the deepest state that keeps the link connectable, not suspend. Stopping
`livi-link` releases the wakelock for a full suspend, at the cost of the car link staying
down until the phone is woken by hand.

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

    cat /run/livi-link/state                   # "connected <timestamp>" or "idle <timestamp>"
    journalctl -u livi-link -f                 # state transitions
    journalctl -u rawlink -f                   # the lines it is matching on

`/etc/default/livi-link` overrides the monitor's knobs: `LIVI_LINK_IDLE_SECS` (how long
rawlink must be silent with the service inactive before idle), `LIVI_LINK_WAKE_REASSERT`
(how often the wake lock and sxmo flag are re-asserted) and `LIVI_LINK_JOURNAL` (e.g.
`-` or `cat /tmp/rawlink.log` to drive it from something else while debugging). The
state machine and `livi-cmd` are covered by
`make -C rawplay livi-cmd && python3 rawplay/test_livi_link.py` on any host.

## Wireless Android Auto / CarPlay (the python helper)

LIVI's wireless paths run `resources/driver/helper/livi-helper.py` inside the chroot: it
talks to the host's **BlueZ** (pairing agent, iAP2/AA Bluetooth profiles, discoverability)
and to **NetworkManager** over the **system D-Bus**, and it drives **hostapd** + **dnsmasq**
to put an AP on the Wi-Fi interface. Inside the chroot that needs two things this
deployment provides:

* the helper's userland: `python3 python3-dbus python3-gi gir1.2-glib-2.0 python3-smbus2
  python3-pip python3-yaml bluez iproute2 iw rfkill hostapd dnsmasq-base procps sudo
  network-manager` (installed in step 4);
* the host **system bus** bound into the chroot (`livi-chroot` mounts `/run/dbus`), without
  which every `dbus.SystemBus()` call fails with
  `Failed to connect to socket /run/dbus/system_bus_socket`;
* on python 3.14 (the 26.04 default) the `livi_asyncio_compat.py` + `.pth` shim restores
  the implicit event loop creation `asyncio.get_event_loop()` lost in 3.14. Without it the
  helper dies on import (`There is no current event loop in thread 'MainThread'`) and the
  app logs `livi-helper.py exceeded max restarts`. The shim is loaded from a `.pth`
  because ubuntu ships its own `/usr/lib/python3.14/sitecustomize.py` that shadows ours.

On the host, `install.sh` writes a `bluetooth.service` drop-in with `--noplugin=sap,midi`
(LIVI's helper does this itself on a normal machine, but its writes land in the chroot):
`sap` otherwise holds RFCOMM channel 8, which Android Auto's AAP wants, and `midi` takes a
128-bit EIR UUID slot CarPlay uses.

The helper only advertises (adapter `Alias` = `carName`, `Discoverable`/`Pairable`, AA
service) once its AP reports ready; if the AP is not ready it logs
`BT advertising held (AP not ready)`.

**The AP takes wlan0.** The wcn36xx firmware advertises no interface combinations, so the
phone cannot be a Wi-Fi client and an AP at once. Enabling wireless AA in LIVI's settings
drops the phone's own Wi-Fi (including ssh) for the duration of the session and puts
`10.10.0.1` on wlan0; disable it again in LIVI's UI to hand the interface back to
NetworkManager. With the single USB port acting as the gadget to the unit, a second Wi-Fi
adapter is not an option while the car link is up.

Because toggling it under the running app upsets LIVI's helper, the deployment parks
`wirelessAaEnabled` **on** and leaves it alone: the AP and the BT advertising come up with
the app and stay up, and `livi-link` never talks to the helper's control socket (see "The
rawplay link state"). `install.sh` merges the key into LIVI's own settings file (never
replacing it), and the app reads it at start, so an ssh upgrade takes effect at the next
livi restart or reboot — the moment wlan0 goes to the AP and the connection drops. From
then on the phone's own Wi-Fi client and ssh are down for as long as the appliance runs;
the sections below assume a local shell.

LIVI's settings UI, its config file, or `livi-cmd` are how it comes down by hand. The
runtime toggle is the same RPC the app sends, only on demand:

    sudo /opt/livi/livi-cmd /opt/livi/rootfs/tmp/cp-bt.sock "set-aa 0"   # wlan0 back to NM
    sudo /opt/livi/livi-cmd /opt/livi/rootfs/tmp/cp-bt.sock "set-aa 1"   # AP back up

A file park changes the boot state (the config is only read at `livi` start):

    sudo sed -i 's/"wirelessAaEnabled": *true/"wirelessAaEnabled": false/' \
        /opt/livi/rootfs/home/user/.config/LIVI/config.json
    sudo systemctl restart livi

Check the helper:

    ps | grep livi-helper                      # spawned by the app once python3 exists
    journalctl -u livi | grep livi-helper      # startup state, profiles registered
    sudo chroot /opt/livi/rootfs /usr/bin/python3 -c \
      'import dbus; print(dbus.SystemBus().get_object("org.bluez","/org/bluez/hci0"))'

With wireless AA up the unit appears to phones as `carName`:

    bluetoothctl show | grep -E "Alias|Discoverable|Pairable"

### The video-plane stride fix (`livistride.c`)

Once the software decoder was in place the wireless video arrived as a diagonally
sheared, "h-sync failure" looking picture while the UI around it was pixel perfect.
waylandsink builds the `wl_shm` buffer from the negotiated caps (`stride 3200` for
800x480 RGBx) but videoconvert hands it row-padded memory: the frame is `3328*480`
bytes, a 256-byte aligned stride. The compositor then reads every row 128 bytes early —
one row's shift per row — which is the shear. `livi.service` sets `LIVI_GST_PRELOAD`
(LIVI's own hook for its GStreamer child) to `livi-stride-fix.so`, which interposes
`gst_wl_shm_memory_construct_wl_buffer()` and calls the real implementation with the
memory's actual stride. Verify with `GST_DEBUG=wl_shm:6`: the log must say
`Creating wl_buffer from SHM of size 1597440 (800 x 480, stride 3328), format RGBx`.
The shim is compiled in the chroot (its gstreamer headers match the bundled 1.28 ABI).

## Audio

The bundled GStreamer only ships `pulsesink` and Chromium's audio is PulseAudio too, so
the app plays through the host's PipeWire by socket. `livi-chroot` binds the host user's
pulse server and cookie at fixed paths and `livi.service` points at them:

    Environment=PULSE_SERVER=unix:/run/pulse/native
    Environment=PULSE_COOKIE=/run/pulse-cookie/cookie

Audio then comes out of the phone's ordinary PipeWire sinks (speaker, headphones). Check
it from inside the chroot:

    sudo chroot /opt/livi/rootfs /usr/bin/env HOME=/home/user \
      PULSE_SERVER=unix:/run/pulse/native PULSE_COOKIE=/run/pulse-cookie/cookie \
      pactl list short sinks

`/SystemVolume/` log lines show LIVI reading and setting the default sink volume. If
`pactl` is missing it logs `is pactl installed?` and audio stays silent.

Playback test, from the chroot (a wav through libpulse, then the app's exact AAC-LC
chain with the bundled gst):

    sudo chroot /opt/livi/rootfs /usr/bin/env HOME=/home/user \
      PULSE_SERVER=unix:/run/pulse/native PULSE_COOKIE=/run/pulse-cookie/cookie \
      paplay /tmp/test.wav
    # with H=<extracted AppImage>/resources/gstreamer/linux-arm64
    sudo chroot /opt/livi/rootfs /usr/bin/env HOME=/home/user \
      PULSE_SERVER=unix:/run/pulse/native PULSE_COOKIE=/run/pulse-cookie/cookie \
      LD_LIBRARY_PATH=$H/lib GST_PLUGIN_PATH=$H/lib/gstreamer-1.0 \
      GST_PLUGIN_SCANNER=$H/libexec/gstreamer-1.0/gst-plugin-scanner \
      $H/bin/gst-launch-1.0 -q filesrc location=/tmp/test.aac ! aacparse ! faad \
      ! audioconvert ! audioresample ! pulsesink

### Who owns PipeWire (the launcher race)

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
(`Restart=on-failure`) and socket-activate pulse on the first client. `livi-chroot` waits
(up to 30 s) for `/run/user/<uid>/pulse/native` before binding it, because `livi.service`
starts before the user manager at boot; without the wait the bind fails and the app comes
up mute until its next restart. To repair an install that already lost the race (or to
restart audio by hand):

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

Afterwards `pactl info` (on the host and in the chroot) must report a default sink and
`systemctl --user status pipewire.service` must say `active (running)`, not `failed
(start-limit-hit)`.

## Boot behaviour

All five units are `WantedBy=multi-user.target` with `Restart=always` and
`StartLimitIntervalSec=0` (restart forever, no rate limit):

    $ systemctl is-active livi-link livi-xvnc livi-weston livi rawlink
    active active active active active

* `livi-link`, `livi-xvnc`, `livi-weston`, `livi` and `rawlink` all enter within a
  few seconds of multi-user; the gadget binds as soon as rawlink starts, `livi-link`
  comes up idle and starts watching the journal, and LIVI brings wireless Android Auto
  up from its config (wlan0 goes to the AP; see the wireless section).
* each program has its own unit, so a crash restarts only that program; killing weston or
  the app brings the whole chain back through the dependencies.
* the AppImage's detached nested compositor is inside `livi.service`'s cgroup (the
  `livi-chroot` supervisor keeps the main process alive for it). stopping is staged:
  the unit runs `KillMode=mixed`, so only the supervisor gets the SIGTERM; it forwards
  that to the app under the compositor and waits up to `LIVI_STOP_GRACE` (10 s) for it
  to quit, so LIVI's own before-quit path runs while wayland is still up, then closes
  the compositor. `TimeoutStopSec=15` SIGKILLs whatever is left, which keeps
  `systemctl restart livi` bounded even when the app wedges. Without this, a stop SIGTERM
  killed the nested compositor at the same instant as the app and a quit path stuck on
  the dead wayland connection held the unit for the default 90 s.

Verification after a reboot:

    systemctl is-active livi-link livi-xvnc livi-weston livi rawlink
    cat /run/livi-link/state                                # idle <timestamp>
    cat /sys/power/wake_lock                                # livi
    sudo cat /sys/kernel/config/usb_gadget/liviraw/UDC      # 7000000.usb
    cat /sys/class/udc/7000000.usb/state                    # not attached / configured
    sudo journalctl -u rawlink -n 5                         # bound to ..., listening on ...
    DISPLAY=:9 ffmpeg -f x11grab -video_size 800x480 -i :9 -frames:v 1 -y /tmp/shot.png

## Manual runs and `livi.sh`

The units do not call `livi.sh` (each program restarts separately), but the stock script
still works on the phone once the AppImage is the chroot wrapper:

    APPIMAGE=/opt/livi/livi-chroot ./livi.sh start     # or a copy with /opt/livi/livi-chroot
                                                       # added to the candidate list
    APPIMAGE=/opt/livi/livi-chroot ./livi.sh stop

While the units run, `livi.sh start` sees the Xvnc port, the weston socket and the
`--user-data-dir=` supervisor already alive and starts nothing.

## Troubleshooting

| symptom | cause / fix |
|---|---|
| `rawstream: capture pipe closed, restarting ffmpeg` forever, 0 frames | `ffmpeg` is not on `PATH` (rawlink execs it for `x11grab`): `apk add ffmpeg`. It is easy to miss because the failure is in the child, not rawlink |
| frames draw line by line and/or audio stutters while the phone's screen is off | the CPUs sit in `cpu-power-collapse` between wakeups, taxing every FunctionFS completion and PipeWire period. `livi-link` should be holding `/dev/cpu_dma_latency` at 0 while a player is connected: check `cat /run/livi-link/state` and `journalctl -u livi-link`; reinstall the current `livi-link-monitor`/unit and `rawlink` (1 MB video pipe, 256 KB ep1 writes); see the host section of `rawplay/README.md` |
| `weston ... failed to create compositor backend` | `weston-backend-x11` not installed |
| `livi-compositor` exits right after `new output` | stale sockets; `livi-chroot` removes `$XDG_RUNTIME_DIR/{livi-compositor.ctrl,wayland-0*}`, keep it that way |
| `xdg_surface geometry (1280x720) is larger than ... (800x480)` | `~/.config/LIVI/config.json` missing or not 800x480 (the inner app reads `$HOME/.config/LIVI`, not `--user-data-dir`) |
| `usbgadget: no usb device controller` looping | phone not in device mode; `rawlink-wait` forces the role and waits for `/sys/class/udc` |
| `systemctl restart rawlink` hangs | old unit had the wait in `ExecStartPre`; it is now in `rawlink-wait` (the main process), so start/restart return immediately even unplugged |
| `systemctl restart livi` hangs in `stop-sigterm` | old unit used the default control-group stop: systemd SIGTERM'd the nested compositor and the app at the same instant, the app's quit path could wedge on the dead wayland connection, and the stop sat the default 90 s. Re-run `install.sh` (or install the current `livi.service` + `livi-chroot` and `systemctl daemon-reload`); a stop is now bounded by `LIVI_STOP_GRACE` plus `TimeoutStopSec` |
| changed `usb.img` not visible to the unit | the LUN holds the old file until the gadget rebinds: `sudo systemctl restart rawlink` |
| LIVI shows no Bluetooth device / `aa-bt initial populate gave up` | the python helper is not running: check `ModuleNotFoundError` in `journalctl -u livi`, install it in the chroot; or the system bus is not bound (`dbus.SystemBus()` error) |
| `[helperSudoers] pkexec not available` | informational: the helper wants to install a sudoers drop-in; running as root in the chroot it does not need it |
| wireless session connects but the screen stays black, `[gst_video] decoder=v4l2h265dec` | the hardware path needs dmabuf and the pixman compositor has none. Remove `libv4l-0t64` (and clear `~/.cache/gstreamer-1.0`) so the probe reports `h264(hw=false sw=true)` and the app uses `avdec_*` |
| black screen, `[CodecCapability] h264(hw=false sw=false)` | the bundled decoder/waylandsink could not load at all. Use the 26.04/Trixie base (24.04 lacks `vaMapBuffer2` and `wl_display_create_queue_with_name`), install `libva2 libva-drm2 libva-x11-2 libssh-4 libgudev-1.0-0` |
| video sheared/smeared ("h-sync failure") while the UI is perfect | waylandsink stride mismatch. Check `GST_DEBUG=wl_shm:6` says `stride 3328`; if not, `LIVI_GST_PRELOAD` is not reaching the app's GStreamer child (see the stride fix above) |
| no audio from LIVI; `pactl info` in the chroot says `Connection refused`; `systemctl --user status pipewire.service` is `failed (start-limit-hit)` | the desktop `pipewire-launcher` autostart raced the systemd user units at login and left `pipewire-0`/`pulse/native` pointing at a dead listener. Re-run `install.sh` (it hides the launcher and repairs the stack) or run the repair block under "Who owns PipeWire" |
| no audio from LIVI, and `paplay` hangs on the host too | `pipewire-pulse` is wedged or gone (`journalctl _UID=10000 \| grep -E 'create_stream_timeout\|pipewire'`). Restart the user audio stack per "Who owns PipeWire" (with `XDG_RUNTIME_DIR=/run/user/10000`). The chroot hears the host's sink through the bound `/run/pulse` socket |
| helper crash-loops with `There is no current event loop in thread 'MainThread'` | python 3.14; install `livi_asyncio_compat.py` + `livi-asyncio-compat.pth` in the chroot's `dist-packages` |
| no audio / `is pactl installed?` | install `libpulse0 pulseaudio-utils`; make sure `livi.service` has `PULSE_SERVER`/`PULSE_COOKIE` and `livi-chroot` bound `/run/pulse` + `/run/pulse-cookie` |
| no Wi-Fi AP / phone never projects | look at `/tmp/livi-hostapd.log` in the chroot and `journalctl -u livi | grep wifi_ap`; `hostapd_cli -p /var/run/hostapd -i wlan0 status` inside the chroot |
| phone's Wi-Fi/ssh is down (or was never reachable) while the appliance runs | expected on wcn36xx (no STA+AP concurrency) and by design: wireless AA is parked on in `livi-config.json` so it is never toggled under the running app. Toggle it off in LIVI's UI, or from a local shell with `/opt/livi/livi-cmd /opt/livi/rootfs/tmp/cp-bt.sock "set-aa 0"`, to get wlan0 back |
| wireless AA never comes up | it is LIVI's own now, not `livi-link`'s: check `journalctl -u livi \| grep livi-helper` (the helper must run) and the AP side (`journalctl -u livi \| grep wifi_ap`, `/tmp/livi-hostapd.log` in the chroot). `wirelessAaEnabled` must be `true` in `livi-config.json`; re-run `install.sh` if it is missing |
| no `/fs/usb0` on the unit | the port must be high speed (jailbroken `io-usb` restart, `usb/homebrew/apps/usbhs.sh`); check `--stick /opt/livi/usb.img` exists |
| phone unreachable after a while | suspend: `livi-link` is not running (it holds the wake lock and the sxmo flag while the appliance is up). `systemctl is-active livi-link`, `cat /sys/power/wake_lock`; only `systemctl stop livi-link` intentionally hands the phone back to sxmo |
| AppImage `cannot execute: required file not found` | ran outside the chroot; use `livi-chroot` (or `APPIMAGE=/opt/livi/livi-chroot`) |
| `unsupported relocation type 1032` | `gcompat` was used; the real glibc chroot is required |
| touch behaves like a mouse: a cursor, drags, taps in the wrong place | weston died or restarted and rawlink is on the XTest fallback. check `journalctl -u livi-weston` for an abort; building `weston-touch.so` without `-DLIVI_WESTON_MAJOR=16` (weston 16) aborts weston on the first touch. rawlink now retries the socket every 2 s, so it also recovers from a weston restart |
| weston core-dumps on restart | weston 16 asserts the touch device list is empty at shutdown; `weston-touch.c` destroys its device in the compositor destroy listener |

## Limits

* **Idle is deep CPU idle, not suspend.** The USB gadget and WiFi must stay enumerated or
  a later rawplay client could never be seen, so `livi-link` keeps the wakelock while it
  runs. Stopping it hands the phone back to sxmo's autosuspend; the car link then stays
  down until the phone is woken by hand.
* **Wireless AA owns wlan0 for the whole appliance run** (no STA+AP concurrency on
  wcn36xx): it is parked on in `livi-config.json` so it is never toggled under the
  running app, which means the phone's own WiFi and ssh are down until it is turned off
  in LIVI's UI (or with `livi-cmd`).
* **No wired CarPlay dongle on the same port.** The single USB port is the gadget to the
  unit; a Carlinkit needs host mode. Wireless (LIVI Link network dongle) is the way to
  have both.
* The stack is CPU-rendered (weston pixman, Electron software GL): fine for 800x480 UI,
  not a GPU pipeline.
* The UI still lists the optional Wi-Fi/BT/audio helpers as dismissed; installing them
  inside the chroot does not give them access to the host's hardware (no system D-Bus),
  which is why they are dismissed rather than installed.
