# rawlink + weston + LIVI as a Buildroot appliance (Orange Pi Zero 2W)

This is the second rawlink deployment: a purpose-built Buildroot image for an
**Orange Pi Zero 2W** (Allwinner H618, aarch64) with a **qemu aarch64 `virt`**
board that proves the software stack on any x86 host. It reuses the phone
deployment's units and the hard-won LIVI/rawlink fixes
(`rawplay/deploy/README.md`, `rawplay/README.md`); the phone deployment is
untouched.

```
 car head unit (QNX, i.MX31, USB host)  <--- USB high speed --->  Orange Pi Zero 2W
   rawplay (video/touch client)                                   rawlink run:
   /fs/usb0 = homebrew stick                                        functionfs vendor iface + mass storage (usb.img)
                                                                    weston_capture_v1 sender
                                                                  weston headless 800x480 kiosk pixman
                                                                  LIVI 8.3.0 (Electron)
```

Divergences from the phone deployment, by design: the H618 is glibc (no
Ubuntu chroot; LIVI runs directly), there is no charger state machine or
`livi-link-monitor`, the rootfs is a read-only squashfs with a small ext4
data partition, and all boot-path drivers are built in.

> Status: the **qemu image builds, boots, reaches T6 and passes
> `tests/smoke.sh`** (measured table below). The **Orange Pi defconfig and
> board files are in place and validated** (the defconfig parses cleanly and
> shares every package with the tested qemu image), but a full board build
> was **not** run in this session and the image has **not** been run on
> hardware (none was available); everything below that concerns the board is
> source-verified and marked as such. Two blocking hardware facts (Wi-Fi/BT, audio) are in "Needs a
> decision" and are the reason the board image cannot yet do a wireless
> session or sound.
>
> The boot path has since been trimmed (see "Boot-time work" below): the
> stock PulseAudio unit that stalled LIVI for ~30 s is masked, the appliance
> units no longer wait on the udev coldplug, the getty comes from a unit with
> no device dependency, the outer Electron launcher is skipped, the
> GStreamer registry is persisted on `/data`, and the board cmdline skips the
> unused-clock teardown. qemu T6 is dominated by Electron under TCG (host
> load moves it a lot); the numbers in the table are the current image on
> this machine, not a hardware prediction.

## Layout

```
rawplay/deploy/buildroot/
  br2-external/
    Config.in, external.desc, external.mk
    configs/               qemu_aarch64_virt_defconfig, orangepi_zero2w[_debug]_defconfig
    board/common/          post-build.sh (machine-id, unit masks, udev/getty policy)
    board/orangepi_zero2w/ linux.fragment, uboot.fragment, boot.cmd[.debug],
                           genimage.cfg, post-image.sh, reference/ (Wi-Fi DT notes)
    board/qemu_aarch64_virt/ linux.fragment, post-image.sh
    package/rawlink/       builds ../../../rawplay/out/rawlink (local site method)
    package/weston-touch/  builds weston-touch.so against the image's libweston
    package/livi/          pins + extracts LIVI 8.3.0, livistride shim, supervisor
    package/livi-appliance/ units, shims, persist/pulse/seed scripts
    package/unit-sim/      qemu's car-side rawplay stand-in
  scripts/                 build.sh, run-qemu.sh, flash.sh, boottime.sh,
                           qemu_boot.py, throughput.sh
  tests/                   smoke.sh, unit-sim/, rawsink.py
```

## Build, flash, run, measure

    scripts/build.sh qemu                       # x86 host, any distro + qemu-system-aarch64
    scripts/build.sh orangepi_zero2w            # release image
    scripts/build.sh orangepi_zero2w debug      # UART console + initcall_debug + ffmpeg

    scripts/run-qemu.sh                         # interactive serial console
    tests/smoke.sh                              # boot, wait for T6, checks, non-zero on failure
    scripts/boottime.sh qemu                    # boot and print the milestone table
    scripts/boottime.sh board serial.log        # parse a captured debug serial log

    scripts/flash.sh build/orangepi_zero2w/images/sdcard.img /dev/sdX

Buildroot is fetched and pinned by `build.sh` (**2026.08**, see "Pin" below)
and the external tree is applied with `BR2_EXTERNAL`; rawlink and
weston-touch are built from the working tree (`local` site), so a `git pull`
plus a rebuild is an update. The LIVI AppImage is fetched, sha256-checked and
extracted at build time; nothing mounts an AppImage at boot.

Build notes: the image needs Python's GObject bindings for LIVI's wireless
helper, and Buildroot's `python-gobject` pulls in `gobject-introspection`,
which builds a host `qemu-user` and host introspection tools. The Mesa
llvmpipe driver pulls in LLVM (host and target), which dominates a clean
build's time; a ccache or a prebuilt toolchain is worth it on repeat builds. The external
tree carries one host-tool patch
(`br2-external/patches/gobject-introspection/`) because `g-ir-scanner` merges
`pkg-config` stderr into its output and on a build host whose `/bin/sh` is
bash+readline the loader's ncurses warning becomes bogus linker arguments;
the patch drops that stderr. This is a build-host fix, not a target change.

## Pin: Buildroot 2026.08, kernel 6.12 LTS / 6.18 LTS, U-Boot 2024.10

* **Buildroot 2026.08**, not the 2026.02 LTS. Reason, verified in the
  sources: the LTS ships weston 14 and **weston 14's headless backend has no
  seat at all** (`libweston/backend-headless/headless.c` has the `fake_seat`
  struct but no `wesyon_seat_init`; `--fake-seat` first appears in 15.0.0).
  Without a seat, `weston-touch.so` can add no touch device, panel keys can
  not be injected, and Electron has no `wl_seat`. 2026.08 ships weston
  15.0.1, which has `--fake-seat` and `weston_capture_v1`; `weston-touch.c`
  is built with `-DLIVI_WESTON_MAJOR=15` and uses its pre-16 API path
  (`weston_touch_create_touch_device()` with 4 arguments, confirmed in
  weston 15.0.1 `libweston/input.c`; 16.0.0 has the 5-argument form).
* **Kernel 6.12.111** for the board (latest 6.12 longterm at the time; the
  upstream defconfig pins 6.12.3) and **6.18.7** for qemu (Buildroot's
  qemu_aarch64_virt base config). U-Boot **2024.10** and TF-A **v2.11**
  (`sun50i_h616`) are upstream Buildroot's orangepi_zero2w choices.
* H618 boot chain is ROM -> SPL -> TF-A BL31 -> U-Boot -> kernel (U-Boot's
  own `orangepi_zero2w_defconfig` and Buildroot's use of
  `BR2_TARGET_UBOOT_NEEDS_ATF_BL31` confirm it). SPL falcon mode is not
  available for sunxi, so U-Boot proper is trimmed instead: `bootdelay=0`
  and `CONFIG_BOOTCOMMAND` loads `boot.scr` from the boot FAT directly, with
  no distro-boot scan.

Package versions that matter (all verified in the 2026.08 tree):
libva 2.24.1 (>= 2.21, the bundled GStreamer's `vaMapBuffer2`),
wayland 1.24.0 (>= 1.23, `wl_display_create_queue_with_name` for
`libgstwaylandsink`), Python 3.14.7 (the asyncio shim is installed),
systemd 258.7, weston 15.0.1, PulseAudio 17.0, hostapd 2.11, BlueZ 5.79.

## Verified hardware and software facts

These were checked against the mainline kernel/device trees, U-Boot, the
extracted LIVI 8.3.0 AppImage and upstream Buildroot 2026.08. Where something
could not be verified it says so.

### USB: USB0 is the USB-C port, on a MUSB controller

* The board DTS (`arch/arm64/boot/dts/allwinner/sun50i-h618-orangepi-zero2w.dts`
  in Linux 6.12+) has `&usbotg { dr_mode = "peripheral"; }` and a comment
  that PHY0's pins go to the **USB-C socket**, that the VBUS pins power the
  device, and that the board can instead be powered via GPIOs, in which case
  port0 *can* act as host. So: **USB-C = USB0 = the car link**, and the board
  must be powered through the header's 5 V pin (or a USB-C power-only cable
  into a separate supply) if the car's port is to be left for data. The
  schematic PDF was not retrievable (Google Drive quota) so the exact header
  pin numbers are **not** recorded here; the DTS comment is the source for
  "power via GPIOs is supported", not a pin map.
* `usbotg` is `compatible = "allwinner,sun50i-h616-musb",
  "allwinner,sun8i-h3-musb"` at 0x05100000 with **no `dmas` property** in
  both mainline 6.12/6.18 and the vendor 6.1 tree.
* `drivers/usb/musb/sunxi.c`'s `.dma_init = sunxi_musb_dma_controller_create`
  is a stub that `return NULL;` — the H3/H616 MUSB path is **PIO only**, no
  DMA, in mainline. The H3/H616 config is `sunxi_musb_hdrc_config_4eps`:
  endpoints 1..4, each with a **single (not double-buffered) 512-byte TX and
  512-byte RX FIFO**; `ram_bits = 11` = 8 KiB of FIFO RAM and the 8 x 512 B
  allocation fits. The gadget's four bulk endpoints are `0x83` EP3-IN,
  `0x04` EP4-OUT, `0x81` EP1-IN, `0x02` EP2-OUT — exactly endpoints 1..4, so
  the descriptor set fits the controller with no endpoint left over.
* **Throughput is the project's biggest risk and is unmeasured.** The raw
  link needs ~11.5 MB/s for 15 fps and ~23 MB/s for 30 fps of 768 000-byte
  frames. PIO with single 512-byte FIFOs can plausibly miss the 30 fps
  number; only a measurement on hardware settles it. `scripts/throughput.sh`
  (board) + `tests/rawsink.py` (host PC) produce the number, and the README
  must be updated with it before this deployment is called done for a car.
  qemu proves nothing about this: dummy_hcd is virtual and the wire is free.

### Wi-Fi/BT: not the guessed AP6256 — it is a Unisoc UWE5622 (AW859A)

* Orange Pi's own build configuration for this board
  (`orangepi-build/external/config/boards/orangepizero2w.conf`) names the
  drivers `uwe5622_bsp_sdio sprdwl_ng sprdbt_tty` and blacklists `bcmdhd`.
  Armbian's extension calls it "Spreadtrum UWE5622 (AW859A)"; the vendor 6.1
  device tree uses SDIO `mmc1` (PG0-PG5) with a PG18 reset and 3.3 V/1.8 V
  rails, and BlueZ attaches over `sprdbt_tty`.
* **Mainline Linux has no driver for it.** A recursive search of Linux
  6.18's tree finds no `uwe`, `sprdwl` or `sprdbt` under
  `drivers/net/wireless` (only unrelated ARM/SPRD platform drivers). The
  driver lives only in vendor trees (`drivers/net/wireless/uwe5622/` in
  Orange Pi's `linux-orangepi`) and in third-party ports. It is not the
  brcmfmac firmware story the brief assumed: no `brcmfmac*.bin`/NVRAM file
  is involved, and no firmware file can be packaged from `linux-firmware`.
  This is a **blocking decision** (below), not a firmware-package tweak.
* The mainline Zero 2W DTS (6.12 and 6.18) does not enable `mmc1` or any BT
  UART. `board/orangepi_zero2w/reference/wifi-uwe5622.dts.fragment` carries
  the exact nodes (from Armbian's sunxi-6.12 patch and the vendor DTS) to
  fold in once a driver package exists; it is deliberately **not applied**,
  because probing an SDIO bus with no driver only costs boot time.

### Audio: the PCM5102A cannot bind on mainline H618 today

* The decided DAC is an external I2S PCM5102A with a `simple-audio-card`
  node. **Mainline has no H616/H618 I2S support at all**: Linux 6.12/6.18's
  `sun50i-h616.dtsi` has no `i2s` node (only the analog `codec@5096000`,
  added for the 24-pin expansion), and `sound/soc/sunxi/sun4i-i2s.c` has no
  H616-compatible string (`sun50i-h6-i2s` is the newest Allwinner entry).
  The pinctrl driver does expose `i2s0` (PA5-9, PI0-4), `i2s2` (PG10-14) and
  `i2s3` (PH5-9), so the blocks exist; the DAI driver binding does not.
* Armbian carries the out-of-tree path: `Sound-for-H616-H618-Allwinner-SOCs`
  (5 400 lines) plus `sun50i-h616-Add-the-missing-digital-audio-nodes`,
  which add an AHUB platform/machine driver and vendor
  `allwinner,sunxi-ahub-daudio` nodes — not the mainline `sun4i-i2s` driver
  and not the `pcm5102a` codec binding. Even with those patches the machine
  driver is the vendor `soundcard-mach`, not `simple-audio-card`.
* Consequence: the board device tree does **not** get a non-binding I2S
  patch. The audio daemon ships a **null sink fallback** and LIVI starts
  and projects regardless; the DAC is a decision point (below). qemu uses
  the null sink and says so. This is exactly the brief's stop condition
  "the PCM5102A cannot be made to work ... on the chosen kernel".
* Header pin numbers were not verified: the Orange Pi schematic PDF is a
  Google Drive file and the folder's download quota was exhausted. The
  pinctrl functions above are the candidate signals; the actual header
  mapping must come from the schematic before anything is wired (do not
  trust a guessed Pi-compatible pinout). This is recorded, not guessed.

### RAM

Orange Pi lists **1 GB / 1.5 GB / 2 GB / 4 GB** LPDDR4 variants (product
page; the name suffix is sometimes just "Zero 2W"). The image assumes the
2 GB variant (Electron needs several hundred MB) and qemu is given 2 GB. No
board measurement was possible. Kernel log's `Memory:` line and `free -m`
are the check on real hardware.

### LIVI 8.3.0

* Release asset: `LIVI-8.3.0-linux-arm64.AppImage`,
  323 365 135 bytes, **sha256
  266b6f0c31e89326c305f4ff678fd7032995eece4666bacef5c137ce881fcb5a**
  (release API digest, and re-computed from the downloaded file). Pinned in
  `package/livi/livi.hash`; 9.x is deliberately not used (dmabufs into the
  pixman nested compositor, black panel).
* Appended squashfs offset is **936456 (0xE4A08)**, the end of the ELF
  section header table (`e_shoff + e_shnum * e_shentsize`); found with a
  byte scan for `hsqs` as well. `extract-appimage.py` computes both, then
  uses `unsquashfs -o`, so the x86 build host never executes the aarch64
  runtime and nothing FUSE-mounts at boot.
* The extracted tree is 832 MB; `livi`'s DT_NEEDED list is the source for
  the image's library set: libasound, libatk-1.0, libatk-bridge-2.0,
  libatspi, libcairo, libcups, libdbus-1, libexpat, libgbm, libgio/glib/
  gobject, libgtk-3, libnspr4/libnss3/libnssutil3/libsmime3, libpango,
  libudev, libX11, libxcb, libXcomposite, libXdamage, libXext, libXfixes,
  libxkbcommon, libXrandr. The image selects all of them (plus Mesa
  softpipe EGL/GLES for `livi-compositor`, whose own bundled libs are inside
  the AppImage).
* `AppRun` sets `PATH=$APPDIR:$APPDIR/usr/sbin:...`,
  `XDG_DATA_DIRS=$APPDIR/usr/share:...`, `LD_LIBRARY_PATH=$APPDIR/usr/lib`
  and `GSETTINGS_SCHEMA_DIR=$APPDIR/usr/share/glib-2.0/schemas`, then execs
  `$APPDIR/livi`. `livi-supervisor` replicates exactly that and execs the
  binary with `--no-sandbox` (the phone unit's args). The detached
  `resources/compositor/bin/livi-compositor -s` and the staged stop are the
  phone's `livi-chroot` logic minus chroot/mounts, because the AppImage
  launcher exits 0 by design.
* The wireless helper (`resources/driver/helper/livi-helper.py` and
  `shared/wifi_ap.py`) was read in the exact pinned tree: the AP path is
  `sudo iw`/`ip`/`hostapd`/`dnsmasq`/`hostapd_cli`/`rfkill`/`pkill`, with
  every call `check=False` or in `try/except`; `nmcli` is only used
  opportunistically (saved profiles, the imager hotspot) and a stub that
  exits 0 is enough; `systemctl` exists for real. Hence the tiny
  `/usr/bin/sudo` shim (drops option words and execs the rest; real sudo is
  not shipped) and the `/usr/bin/nmcli` stub, and no NetworkManager and no
  wpa_supplicant anywhere. Python 3.14 gets
  `livi_asyncio_compat.py` + `.pth` (installed only when the target Python
  is >= 3.14).
* `pactl` is required by LIVI's `SystemVolume` (grep of `app.asar`), so the
  image runs PulseAudio 17 as a system daemon with a default sink always set.
* `livi-config.json` seeds both `$HOME/.config/LIVI/config.json` (inner app)
  and `$HOME/.config/LIVI-vnc/config.json` (`--user-data-dir`); both carry
  800x480 or the kiosk fullscreen check rejects the window.

## Image design and why

* **Boot order**: `livi-persist` (mount /data, set up `$HOME`, machine-id)
  -> `rawlink` (gadget bound and streaming; FIFO priority 20 via systemd
  `CPUSchedulingPolicy=fifo`, the `chrt -f` the brief asks for) and
  `livi-pulse` in parallel -> `livi-weston` -> `livi`. rawlink retries the
  wayland socket by itself, so weston does not gate the gadget; the unit
  sees a UDC and a stick as early as the kernel allows. Every appliance unit
  sets `DefaultDependencies=no` and an explicit `local-fs.target` order:
  with the defaults they also waited on `systemd-udev-trigger`, and that
  coldplug costs seconds on this CPU for devices the appliance never asks
  udev about. `livi-persist` finds the data partition by its fixed `/dev`
  node (`/dev/vda2`, `/dev/mmcblk0p3`) so it needs no udev-created
  `/dev/disk/by-label` symlink.
* **Session start**: `livi-supervisor` starts
  `resources/compositor/livi-compositor -s` directly instead of exec'ing the
  outer Electron launcher. The shipped launcher's only job (`kn()` in
  `out/main/main.js`) is to write the config - which the inner app repeats -
  and spawn that same compositor with the same command string and
  environment, so the direct start removes one full Electron cold start
  (qemu's TCG clock charges ~12 s for it) with no change to what runs.
  `LIVI_USE_OUTER_LAUNCHER=1` restores the shipped path.
* **udev and the console**: `systemd-udev-trigger` is masked; the kernel has
  every boot-path driver built in, devtmpfs carries the nodes, and udevd
  still runs for live events (the car plugging in, a card inserted later).
  The stock `serial-getty@.service` pulls in its own `.device` unit and would
  sit on the 90 s device timeout once the coldplug is gone, so the qemu and
  debug images get `livi-console-getty.service`, an agetty with no device
  dependency (the release image boots no getty at all). `systemd.getty_auto=0`
  and masks of the stock tty instances keep any generated getty out.
* **GStreamer registry**: `livi.service` points `GST_REGISTRY` at
  `/data/livi-config/gst-registry.aarch64.bin` (`GST_REGISTRY_UPDATE=no`).
  The default cache under `$HOME` is tmpfs and thrown away each boot, so
  GStreamer rescanned every plugin on every start; with the persistent path
  only the first boot after a flash scans. The shipped `config.json` still
  seeds the profile on `/data`, so Chromium's own caches persist too.
* **Rootfs**: squashfs, zstd, 128 KiB blocks, mounted `ro`; tmpfs `/run`,
  `/tmp`, `/var` (Buildroot's systemd `/var` factory) and `/home/user`; a
  128 MB ext4 partition labelled `livi-data` mounted `noatime,commit=1`
  holds only `$HOME/.config` (bind) and `/var/lib/bluetooth` (bind) plus a
  persistent machine-id. Power can be cut at any time: the rootfs can not
  be corrupted and the persistent partition is journaled with a 1 s commit.
* **Kernel compression**: uncompressed `Image`. The boot media is an SD
  card whose read speed is the same order as the kernel size, and arm64
  boot decompression is serial work; the brief says choose by measurement,
  and on the bench that is a one-line defconfig change
  (`BR2_LINUX_KERNEL_IMAGEGZ`) — noted as a follow-up rather than guessed.
* **Kernel config**: the upstream arm64 defconfig plus a small fragment:
  gadget stack built in (`USB_CONFIGFS`, `F_FS`, mass storage,
  `USB_MUSB_GADGET`), squashfs/zstd, `NULL_TTY`, Bluetooth/802.11 core for
  when the UWE5622 driver lands, and tracing/debug off. No module is loaded
  in the boot path. `quiet loglevel=0`, `rootwait ro`,
  `random.trust_cpu=on`, `clk_ignore_unused pd_ignore_unused` (skip the
  sunxi late init that walks every unused clock and power domain) and
  `console=ttynull` in the release image; the debug image keeps the UART
  with `loglevel=7 initcall_debug`, which is what `boottime.sh board`
  parses. Both set `systemd.getty_auto=0` because the stock serial getty
  would otherwise wait on a `.device` unit the masked coldplug never
  creates (`console=ttynull` gets one too).
* **U-Boot**: board defconfig + fragment (`BOOTDELAY=0`, direct
  `boot.scr` load; no network/fastboot in the board defconfig to start
  with). The SPL/TF-A stages are untouched.
* **Audio**: PulseAudio 17 system daemon, one process, started before LIVI;
  `livi-pulse` writes the DAC sink first when `/proc/asound/cards` names a
  PCM5102/livi card and the null sink otherwise, then the null sink; the
  first sink loaded is PulseAudio's default with no state to restore
  (LIVI's `pactl` calls need a default sink at startup). The stock
  `pulseaudio.service` is masked: it would start a second, unconfigured
  daemon that takes `/run/pulse/pid`, and its `ExecStartPost` retry loop
  then held `livi.service` for ~30 s. No PipeWire: more processes for no
  benefit on an appliance with no session.
* **LIVI start-up**: the `--wayland` capture needs no ffmpeg; the release
  image has no ffmpeg. `--disable-dev-shm-usage`, `--ozone-platform` and
  GPU flags were left at the phone's validated environment: 8.3.0's inner
  app already uses `ozone-platform=wayland` (grep of `app.asar`) and the
  pixman nested compositor is the validated renderer. `--disable-gpu` was
  not applied because the phone's working configuration does not use it,
  and the brief says only keep experiments that measure better. Mesa is
  built with **llvmpipe** (the phone chroot's software GL) as well as
  softpipe, because Chromium's GPU process rejects softpipe's GLES3 EGL
  configuration and LIVI's app then exits/restarts under qemu; llvmpipe is
  the configuration that is actually validated on the phone. The unit
  also sets `APPIMAGE=/opt/livi/livi-inner`: LIVI's outer launcher builds
  the inner Electron command from `$APPIMAGE`, and on the phone that is the
  AppImage runtime whose AppRun adds `--no-sandbox`. With the extracted tree
  the path would be the raw binary, which fatals as root; the wrapper is the
  same shim AppRun would have been, and it adds the flag.
* **qemu board**: same rootfs, same units, same package set; only the
  board parts differ. `dummy_hcd` gives a real configfs/FunctionFS UDC and
  a virtual host controller in the same guest (verified: it supports high
  speed, and rawlink binds it exactly as it does MUSB); `unit-sim` is the
  car. `mac80211_hwsim` provides wlan0/wlan1 for hostapd and the helper;
  `hci_vhci` exists for BlueZ but qemu cannot model a phone's Bluetooth
  controller (BlueZ comes up without a default controller). A `virtio-rng`
  device avoids the entropy stall Chromium otherwise risks.

## Boot measurements

Everything here is measured by `scripts/boottime.sh` / `tests/smoke.sh`
from a `systemd-analyze` run inside the guest plus the guest's own
`/run/livi-boot.log` markers; the qemu numbers are **relative only** (the
UDC is virtual, the wire free, and the "hardware" is a TCG-emulated
Cortex-A76). Real board numbers require the debug image and hardware.

T0 power, T1 kernel entry, T2 init, T3 gadget bound+rawlink, T4 weston
socket, T5 first frame, T6 non-blank LIVI UI.

Measured by `tests/smoke.sh` on this machine (Buildroot 2026.08, kernel
6.18.7, qemu 11 TCG `-cpu max`, 4 vCPU, 2 GiB; serial log
`build/smoke-console.log`). "guest" is the kernel/`/proc/uptime` clock,
"host" is mapped onto qemu's process start (T0) with the offset of the first
printk timestamp. The coordinated change is hard to time on a shared
machine - this host runs a desktop, and TCG timings move by 2-4x with its
load - so the table is one `tests/smoke.sh` run (all checks pass, warm
`/data`):

| id | milestone | guest s | host s |
|---|---|---|---|
| T0 | power on / qemu start | - | 0.000 |
| T1 | kernel entry | 0.000 | 3.996 |
| T2 | rootfs mounted, init running | 1.001 | 4.996 |
| T3 | gadget bound to the UDC, rawlink running | 5.140 | 9.136 |
| T4 | weston wayland-livi socket exists | 5.710 | 9.706 |
| T5 | first FRAME sent | 6.500 | 10.496 |
| T6 | LIVI UI on the weston output | 39.880 | 43.876 |

Before this work the documented run was T2 1.0 s, T3/T4 10.2 s and T6
77.3 s with `multi-user.target` at 35.9 s; the appliance chain is now
~4 s to weston and ~5 s to the gadget instead of ~10 s, and T6 lost the
~30 s PulseAudio stall plus the outer Electron start.

`systemd-analyze` in the same run: **multi-user.target reached after 4.4 s
in userspace** and `systemctl is-system-running` is `running` (the old
image reached multi-user at 35.9 s only after the broken PulseAudio restart
finished). The critical chain is the persistent mount and the marker unit
that measures T3/T4:

```
multi-user.target @4.442s
└─livi-markers.service @3.569s +868ms
  └─livi-weston.service @3.410s +112ms
    └─livi-persist.service @3.003s +377ms
      └─local-fs.target @2.994s
        └─home-user.mount @3.932s
          └─local-fs-pre.target @2.331s
            └─systemd-tmpfiles-setup-dev.service @2.196s +131ms
```

T2 to T5 is ~5.5 s; T5 to T6 is **all Electron under TCG**: Chromium
starts, fails to create a GLES3 context (see below), falls back to software
rasterization and paints the first non-blank frame. That is the number the
board's SD card and CPU replace, and the target there is the brief's 10 s
(the phone's LIVI starts in a few seconds on real hardware).

### Boot-time work, and why each piece exists

The boot path was changed only where it could be verified from the qemu
image; every item was either removed entirely or kept as a toggle.

* **One PulseAudio, one sink policy.** `pulseaudio.service` (from the
  package) ran first, took `/run/pulse/pid`, and made `livi-pulse` fail to
  create its own; `livi-pulse`'s retrying `livi-default-sink` ExecStartPost
  then sat for ~30 s before systemd marked it failed, and `livi.service`
  (`After=livi-pulse.service`) did not start until then. The stock unit is
  now masked, and the sink policy is "the first sink PulseAudio loads is the
  default" - no `pactl` call, no retry loop, no `module-always-sink`
  (which would create `auto_null` first). This is the single biggest fix.
* **Off the sysinit path.** `livi-persist`, `rawlink`, `livi-weston`,
  `livi-pulse` and `livi` set `DefaultDependencies=no` with an explicit
  `After=local-fs.target` and `Before=shutdown.target`, so they no longer
  wait for `systemd-udev-trigger`/`sysinit.target`.
* **No coldplug.** `systemd-udev-trigger` is masked. It re-emitted a uevent
  for every device on the system (2-3 s here); every boot-path driver is
  built in, devtmpfs has the nodes, and udevd still processes live events.
  `livi-persist` mounts the data partition by fixed node rather than the
  udev `by-label` symlink.
* **A getty without a device unit.** The stock `serial-getty@.service`
  `BindsTo=dev-%i.device`; without the coldplug that unit never appears and
  the getty held `multi-user.target` for the 90 s device timeout.
  `livi-console-getty.service` (qemu and debug images only) starts agetty
  straight on the devtmpfs node; `systemd.getty_auto=0` plus masks of the
  stock tty instances keep generated gettys out.
* **Skip the outer Electron.** `livi-supervisor` starts
  `resources/compositor/livi-compositor -s` with the exact command and
  environment the outer launcher's `kn()` builds, saving one Electron cold
  start (~12 s under qemu TCG).
* **Persistent GStreamer registry.** `GST_REGISTRY` on `/data` with
  `GST_REGISTRY_UPDATE=no`: the ~4 s plugin scan happens only on the first
  boot after a flash, not on every boot. The Chromium profile on `/data`
  already made later boots cheaper; this is the GStreamer half of that.
* **Units that only boot work.** `cups.{service,path,socket}` (GTK printing
  only), `systemd-network-generator.service` (no networkd) and
  `systemd-udev-load-credentials.service` (no credentials) are masked.
* **Board cmdline.** The release boot script adds
  `clk_ignore_unused pd_ignore_unused` (skip the sunxi unused-clock/power-
  domain teardown) and `systemd.getty_auto=0`, on top of the existing
  `quiet loglevel=0 console=ttynull`.

The harness records the slowest `initcall_debug` entries and
`systemd-analyze blame`/`critical-chain`; the qemu kernel does not boot with
`initcall_debug` (board debug image only), and `blame` output on the serial
console is interleaved with live unit output, so the critical chain above is
the usable part. The qemu kernel is the minimal Buildroot config, not the
board's trimmed arm64 defconfig, so none of this is a board prediction.

### Initcalls and userspace blame

`boottime.sh` prints the slowest `initcall_debug` entries (debug images) and
`systemd-analyze blame` / `critical-chain`, and `qemu_boot.py`'s check block
also records `systemctl is-system-running` and `systemctl list-jobs` so a
boot that never reaches multi-user is visible (it caught the stock getty
above). qemu's minimal kernel does not boot with `initcall_debug`, so that
column is board/debug-only.

## What the qemu run proves, and what it does not

**Proves**: the rootfs layout and read-only/tmpfs/persist split, systemd
boot order and unit graph, the FunctionFS gadget shape and functionfs
binding, rawlink's capture/conversion/framing against a real compositor and
the wire protocol against an independent reader (`unit-sim`), the weston
headless+kiosk+pixman + `weston-touch` module stack, that the pinned LIVI
AppImage starts under this glibc and renders a non-blank frame, PulseAudio
plus the null sink, and the helper shims (`sudo`, `nmcli`, asyncio).

**Does not prove**: H618 USB throughput (virtual UDC), any real USB
electrical/timing behaviour, Wi-Fi/BT (UWE5622 driver absent; hwsim is not
a phone), I2S/PCM5102A audio (no driver, no card), real SD-card or CPU
timing, or that the car's i.MX31 rawsplay accepts any of it. The smoke
test's mass-storage check exercises usb-storage and the read-only LUN
inside the guest.

Two qemu-specific observations worth knowing:

* Chromium's GPU process needs a GLES3-capable software EGL. With only Mesa
  softpipe it fails (`eglCreateContext ES 3.0 failed with EGL_BAD_ATTRIBUTE`)
  and, after painting the UI, LIVI's app exits and restarts every ~20 s.
  T6 still lands and every smoke check passes, but this is why the image
  builds Mesa **llvmpipe** (`BR2_PACKAGE_MESA3D_LLVM` +
  `GALLIUM_DRIVER_LLVMPIPE`), which is what the phone's chroot's
  `libgl1-mesa-dri` provides. Softpipe is kept as a fallback.
* `dummy_hcd` (the virtual host controller in the same guest) has oopsed its
  hrtimer once under sustained bulk traffic; `tests/smoke.sh` retries once
  when the console log says `Kernel panic` and fails on anything else. It is
  a harness flake, not the appliance: the UDC/FunctionFS path itself is
  upstream kernel code exercised identically on the board.
* The first boot after a fresh `disk.img` (an empty `/data`) can leave LIVI
  mapped but unpainted under TCG: Chromium's GPU init and first-paint
  against a cold profile are slow enough that the emulated timing sometimes
  wins the race. A second boot uses the persistent profile (and the
  persistent GStreamer registry) and T6 lands every time measured. This is
  the qemu clock, not a board result; the `qemu_boot.py` command line sets
  `systemd.setenv=LIVI_INNER_ARGS=--disable-gpu` for the qemu device's
  benefit (the board keeps the phone-validated GPU path).

## Hardware bring-up checklist (no board was available here)

Everything in this list is unexecuted. It is the order that gets the most
information out of a first power-on.

1. Power the board from the 40-pin header's 5 V/GND (the DTS comment says
   GPIO power is supported and leaves port0 free), **not** from the USB-C
   socket, so the USB-C port can be the car link. Check `Memory:` in the
   debug UART log for the RAM variant.
2. Flash the debug image (`scripts/build.sh orangepi_zero2w debug`) and open
   `ttyS0` at 115200. Confirm the boot chain in the log (SPL, BL31, U-Boot),
   then `scripts/boottime.sh board serial.log` for the milestone table.
   T5/T6 are only meaningful with a rawplay or the bench sink attached.
3. Put the release image on a card (`scripts/flash.sh`), connect USB-C to a
   Linux bench host, and run `tests/rawsink.py` on the host while
   `scripts/throughput.sh` runs on the board. Record the sustained MB/s and
   frame rate; the brief's threshold is 11.5 MB/s for 15 fps, 23 MB/s for
   30 fps. If it is short, stop and use "Needs a decision" item 3.
4. With the car: confirm the gadget enumerates (the unit's `io-usb` must be
   in high speed, `usb/homebrew/apps/usbhs.sh`), `/fs/usb0` mounts and the
   launcher's rawplay button starts the player; then check the panel shows
   LIVI, touch round-trips, and the panel keys tap their LIVI actions.
5. Audio: `aplay`/`speaker-test` cannot work until item 2 of "Needs a
   decision" is resolved; the null sink proves the daemon and `pactl` path.
6. Wireless: nothing to test until item 1 is resolved. The helper, hostapd
   and dnsmasq are installed and their shims are exercised in qemu, but the
   UWE5622 has no driver.

## Needs a decision

1. **Wi-Fi/BT driver (blocks wireless AA/CarPlay and Bluetooth pairing).**
   The module is a Unisoc UWE5622/AW859A with no mainline driver. Options,
   cheapest first:
   a. add a br2-external package that builds a forward-ported `sprdwl_ng` +
      `sprdbt_tty` against 6.12 (Armbian/misuzu/OpenWrt carry 6.6-6.12
      ports) and ship the BT attach service; risk: out-of-tree SDIO driver
      quality, firmware blobs must be vendored, unknown boot-time cost.
   b. choose a different board with a mainline Wi-Fi/BT part (AP6256-class)
      — the deployment stays the same.
   c. accept wired-only, which for LIVI means no phone session at all; not
      useful.
   The source evidence above is why this is a decision and not done here.
2. **I2S/PCM5102A (blocks audio).** Mainline has no H616 I2S DAI. Options:
   a. port Armbian's H616 AHUB series and wire the PCM5102A to the vendor
      machine driver (not `simple-audio-card`), which is a real driver port;
      b. use the H618's internal `codec@5096000` LINEOUT instead of the DAC
      if the Zero 2W routes it (needs the schematic; not established);
   c. ship without sound and accept null-sink-only, which loses the
      projection audio.
3. **MUSB throughput.** If the bench measurement in `scripts/throughput.sh`
   is below ~11.5 MB/s the 15 fps minimum fails; the response would be
   switching to the MUSB host-capable port wiring or a different SoC/board
   with a dwc2/dwc3 UDC. Unmeasured here.
4. **No serial console in release vs recovery.** As asked, the release
   image has `console=ttynull` and no getty; the debug defconfig is the
   recovery path. If the owner wants a physical recovery button instead,
   that is a hardware decision.
5. **Root services and anonymous PulseAudio.** LIVI, rawlink and weston run
   as root (the phone ran weston as `user`), and the system PulseAudio
   socket is `auth-anonymous` because no login session exists. The AP has no
   shell and no root login; still, if the threat model changes, both are the
   places to revisit.

## Limits / future work

* The board image is untested on hardware in this session.
* Board boot time is not yet measured; the qemu table says nothing about
  the 10 s target. The T5->T6 gap in qemu is Electron under TCG; on the
  board, Electron's startup on the SD card is the thing to profile first.
  The shipped image starts with a fresh `/data`, so the pre-flash boot is
  the cold-profile case; the readahead/pre-warm experiments are still the
  next lever there.
* `initcall_debug` parsing is only exercised by debug images; the qemu
  kernel does not use it.
* Wi-Fi/BT and audio are blocked above; the helper's hostapd/BT paths are
  therefore unexercised outside unit-sim's fake gadget.
* A pre-warmed V8 code cache / readahead of the Electron tree
  (`vmtouch`-style) is not implemented; the brief lists it as an
  experiment to run once the board is on a bench. The persistent Chromium
  profile and GStreamer registry on `/data` already remove the repeated
  per-boot caches after the first boot.
* The `usb.img` in a clean checkout is a stand-in (MBR+FAT with the tracked
  stick base and `homebrew/apps/rawplay.sh`); run `make stick` at the repo
  root before flashing a car-bound card so the unit gets the real launcher,
  HMI assets and the built QNX rawplay.
