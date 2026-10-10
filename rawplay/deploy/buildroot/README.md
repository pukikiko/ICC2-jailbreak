# rawlink + weston + LIVI-Lite as a Buildroot appliance (Orange Pi Zero 2W)

This is the second rawlink deployment: a purpose-built Buildroot image for an
**Orange Pi Zero 2W** (Allwinner H618, aarch64) with a **qemu aarch64 `virt`**
board that proves the software stack on any x86 host. It reuses the phone
deployment's units and the hard-won LIVI/rawlink fixes
(`rawplay/deploy/README.md`, `rawplay/README.md`).

```
 car head unit (QNX, i.MX31, USB host)  <--- USB high speed --->  Orange Pi Zero 2W
   rawplay (video/touch client)                                   rawlink run:
   /fs/usb0 = homebrew stick                                        functionfs vendor iface + mass storage (usb.img)
                                                                    weston_capture_v1 sender
                                                                  weston headless 800x480 kiosk pixman
                                                                  LIVI-Lite (Rust + Slint, from source)
```

The image uses **LIVI-Lite** (pukikiko/LIVI-Lite), the Electron-free fork of
LIVI: the upstream Rust core, a Slint UI and the Rust nested compositor,
cross-built from the `LIVI-Lite` checkout next to this repo by
`package/livi-lite` with Buildroot's prebuilt Rust and the image's own
GStreamer/Wayland. There is no AppImage, no Electron/Chromium and no
supervisor script: `livi.service` runs `/opt/livi/livi-core`, which starts the
compositor, UI, GStreamer host and helper itself.

Divergences from the phone deployment, by design: the H618 is glibc (no
Ubuntu chroot), there is no charger state machine or
`livi-link-monitor`, the rootfs is a read-only squashfs with a small ext4
data partition, and all boot-path drivers are built in.

> Status: the **qemu image builds, boots, reaches T6 and passes
> `tests/smoke.sh`** with LIVI-Lite (measured table below). The **Orange Pi
> defconfig and board files are in place and validated** (the defconfig parses
> cleanly and shares every package with the tested qemu image), but a full
> board build was **not** run in this session and the image has **not** been
> run on hardware (none was available); everything below that concerns the
> board is source-verified and marked as such. One blocking hardware fact
> (audio) is in "Needs a decision" and is the reason the board image cannot
> yet do sound. Wi-Fi/BT now ships in the image (below) but is likewise
> untested on hardware.
>
> The boot path is trimmed (see "Boot-time work" below): the stock PulseAudio
> unit that stalled LIVI for ~30 s is masked, the appliance units no longer
> wait on the udev coldplug, the getty comes from a unit with no device
> dependency, the GStreamer registry is persisted on `/data`, and the board
> cmdline skips the unused-clock teardown. With LIVI-Lite the T5->T6 gap is no
> longer an Electron cold start (it was ~30 s under qemu TCG); the numbers in
> the table are relative qemu measurements, not a hardware prediction.

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
    package/livi-lite/     cross-builds pukikiko/LIVI-Lite from source (local site)
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
and the external tree is applied with `BR2_EXTERNAL`; rawlink, weston-touch
and LIVI-Lite are built from the working trees (`local` site), so a `git pull`
plus a rebuild is an update. LIVI-Lite's crate downloads are pinned by each
workspace's `Cargo.lock` and cached in `$(DL_DIR)/br-cargo-home`; the Rust
toolchain itself is Buildroot's prebuilt `host-rust-bin`.

Build notes: the Mesa llvmpipe driver pulls in LLVM (host and target), which
dominates a clean build's time; a ccache or a prebuilt toolchain is worth it
on repeat builds. With LIVI-Lite the image no longer needs Python or
`gobject-introspection` at all (the helper is Rust), so the old
`gobject-introspection` host-tool patch in `br2-external/patches/` is no
longer exercised; it is kept because the external tree still supports the
older Electron package set.

## Pin: Buildroot 2026.08, kernel 6.12 LTS / 6.18 LTS, U-Boot 2024.10

* **Buildroot 2026.08**, not the 2026.02 LTS. Reason, verified in the
  sources: the LTS ships weston 14 and **weston 14's headless backend has no
  seat at all** (`libweston/backend-headless/headless.c` has the `fake_seat`
  struct but no `wesyon_seat_init`; `--fake-seat` first appears in 15.0.0).
  Without a seat, `weston-touch.so` can add no touch device, panel keys can
  not be injected, and the UI has no `wl_seat`. 2026.08 ships weston
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
LIVI-Lite checkout and upstream Buildroot 2026.08. Where something
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

### Wi-Fi/BT: Unisoc UWE5622 (AW859A), shipped out of tree on mainline

* Orange Pi's own build configuration for this board
  (`orangepi-build/external/config/boards/orangepizero2w.conf`) names the
  drivers `uwe5622_bsp_sdio sprdwl_ng sprdbt_tty` and blacklists `bcmdhd`.
  Armbian's extension calls it "Spreadtrum UWE5622 (AW859A)"; the vendor 6.1
  device tree uses SDIO `mmc1` (PG0-PG5) with a PG18 reset and 3.3 V/1.8 V
  rails, and BlueZ attaches over `sprdbt_tty`.
* **Mainline Linux has no driver for it**, so the image carries one:
  `package/uwe5622` builds the pinned `armbian/uwe5622` tree (the unified
  tree Armbian build-tests on 6.12.y, so no version patches) **out of tree**
  against the image's own 6.12.111 kernel build dir. The kernel package
  itself is never recompiled for this: only the three modules land in
  `/lib/modules/.../extra` (`uwe5622_bsp_sdio`, `sprdwl_ng`, `sprdbt_tty`),
  loaded via `/etc/modules-load.d/uwe5622.conf`. It is not the brcmfmac
  firmware story the brief assumed: no `brcmfmac*.bin`/NVRAM file is
  involved.
* The SDIO bus is the one DTS-only kernel patch
  (`board/orangepi_zero2w/kernel-patches/`): `mmc1` with the two always-on
  regulators and the `mmc-pwrseq-simple` PG18 reset, mirroring Armbian's
  sunxi nodes and the vendor DTS (the pwrseq clock uses the mainline
  `sun6i-rtc` binding). `board/orangepi_zero2w/reference/` keeps the notes
  this was derived from.
* Firmware is `wcnmodem.bin`, decoded at package build time from the
  `.hex` the driver ships (the driver's compiled-in arrays are stubs, so
  without this file the chip never boots). It is installed both as
  `/lib/firmware/wcnmodem.bin` (the `request_firmware` path the marlin
  boot uses) and `/lib/firmware/uwe5622/wcnmodem.bin`.
* Bluetooth is HCI-H4 over the same SDIO bus (no UART): `sprdbt_tty`
  presents `/dev/ttyBT`, `uwe5622-bluetooth.service` binds it with
  `btattach -P h4`, and `bluetooth.service` is enabled for `bluetoothd`.
  Wi-Fi station + AP come from the same chip: `wpa_supplicant` joins
  networks for bench work while the helper's `hostapd`/`dnsmasq`/`iw`
  path (already in the image) serves the phone-facing AP.
* **Untested on hardware** (no board in this session): the module build
  against 6.12.111 is verified, the patched DTS compiles, and the
  defconfig parses, but first power-on still has to show `wlan0` +
  `hci0`. The driver is a vendor BSP port ("the driver is trash" per its
  own porters): expect flakiness, not Intel quality.

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
2 GB variant (the native stack is small; 1 GB works, 2 GB is headroom) and qemu is given 2 GB. No
board measurement was possible. Kernel log's `Memory:` line and `free -m`
are the check on real hardware.

### LIVI-Lite

* Source: the `pukikiko/LIVI-Lite` checkout next to this repo (override
  `LIVI_LITE_SRC=`), built by `package/livi-lite` with Buildroot's prebuilt
  Rust (`host-rust-bin`, Rust 1.97.1) for `aarch64-unknown-linux-gnu`. The
  build is one shared script (`rawplay/deploy/livi-lite-build.sh`) that
  compiles four workspaces - `livi-gst-host`, `livi-compositor`, `livi-core`
  + `livi-helperd`, `livi-ui` - with `--locked` and stages the installed
  layout `Resources::installed()` expects:
  `/opt/livi/livi-core`, `/opt/livi/livi-ui`,
  `/opt/livi/resources/{driver/livi-helperd,gst-host/livi-gst-host,compositor/livi-compositor}`
  plus the root templates from `assets/linux`.
* Runtime dependencies are the image's own libraries, not a bundle:
  GStreamer 1.x (system), wayland/libxkbcommon, libudev (systemd), EGL
  (Mesa llvmpipe on qemu, the board's GPU/llvmpipe on hardware), and the
  GStreamer plugins its pipelines use - `gst1-plugins-bad` (waylandsink,
  h264/h265 parse, faad), `gst1-plugins-good` (pulsesink, volume, aacparse,
  RTP), `gst1-plugins-base` (convert/resample/opus) and `gst1-libav`
  (`avdec_h264`/`avdec_h265`, the software fallback).
* There is no AppImage, no Electron/Chromium, no python helper and no
  supervisor script. `livi-core` starts the nested compositor, the Slint UI,
  the GStreamer video host and the helper itself and stays in the foreground;
  its SIGTERM handler stops them in order. That removes the two boot costs
  the Electron build paid (the AppImage/outer-launcher cold start and
  Chromium's own startup) and most of the 832 MB prebuilt tree.
* The udev rule livi-core installs at first run (`99-LIVI.rules` + the touch
  filter) is pre-installed at image build time: the rootfs is a read-only
  squashfs, so the runtime install would otherwise fail on every boot and
  livi-core would restart itself once trying to pick it up.
* The wireless helper is `livi-helperd` (Rust) running as root through the
  tiny `/usr/bin/sudo` shim: the AP path is still
  `iw`/`ip`/`hostapd`/`dnsmasq`/`rfkill`/`pkill`; `nmcli` is the
  opportunistic stub; D-Bus is the system bus. `pactl` is required by
  `SystemVolume`, so the image runs PulseAudio 17 as a system daemon with a
  default sink always set.
* `livi-config.json` seeds `$HOME/.config/LIVI/config.json` (and the old
  `LIVI-vnc` dir, so an upgrade from an Electron install keeps its
  settings); it carries 800x480 for the screen and the projection
  (`projectionWidth`/`projectionHeight`), so Android Auto negotiates the
  800x480 tier and the kiosk fullscreen check accepts the window.
  `livi.service` also sets `LIVI_UI_SIZE=800x480`, which fresh installs
  size their projection defaults from (and `livi-ui` opens at).

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
* **Session start**: `livi.service` runs `/opt/livi/livi-core`, which starts
  `resources/compositor/livi-compositor` itself (the fork's design: one owner
  for the compositor, UI and helper). There is no outer launcher and no
  supervisor: the unit supervises the whole tree through one process.
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
  only the first boot after a flash scans.
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
  image has no ffmpeg. `livi.service` sets `LIVI_RESOURCES`, `LIVI_KIOSK`,
  `LIVI_UI_SIZE=800x480`, `WAYLAND_DISPLAY=wayland-livi`, `PULSE_SERVER` and the persistent
  `GST_REGISTRY`; everything else is the defaults. Mesa is built with
  **llvmpipe** (plus softpipe as the fallback): the Rust compositor renders
  through system EGL and llvmpipe is the software configuration that works
  both here and on the phone (Adreno GL on hardware). There is no Electron
  flag set left to carry: no `--no-sandbox`, no `--disable-gpu`, no
  `ozone-platform`, no `APPIMAGE` shim.
* **qemu board**: same rootfs, same units, same package set; only the
  board parts differ. `dummy_hcd` gives a real configfs/FunctionFS UDC and
  a virtual host controller in the same guest (verified: it supports high
  speed, and rawlink binds it exactly as it does MUSB); `unit-sim` is the
  car. `mac80211_hwsim` provides wlan0/wlan1 for hostapd and the helper;
  `hci_vhci` exists for BlueZ but qemu cannot model a phone's Bluetooth
  controller (BlueZ comes up without a default controller). A `virtio-rng`
  device avoids an entropy stall in the helper's crypto at first start.

## Boot measurements

Everything here is measured by `scripts/boottime.sh` / `tests/smoke.sh`
from a `systemd-analyze` run inside the guest plus the guest's own
`/run/livi-boot.log` markers; the qemu numbers are **relative only** (the
UDC is virtual, the wire free, and the "hardware" is a TCG-emulated
Cortex-A76). Real board numbers require the debug image and hardware.

T0 power, T1 kernel entry, T2 init, T3 gadget bound+rawlink, T4 weston
socket, T5 first frame, T6 non-blank LIVI UI. **T6u** is the same endpoint
("LIVI is on the panel") read from the guest's own console instead of the
rawplay wire: Electron logs `[kiosk] enter:` when its window is presented,
LIVI-Lite logs `[core] UI started` when the Slint UI starts. `scripts/
boottime.sh` prints it when present.

The T5/T6 wire is a test instrument: `unit-sim` on `dummy_hcd` inside qemu.
On some qemu/kernel hosts that virtual USB host drops or corrupts the first
MODE message (the guest console shows `FRAME before MODE, ignored`), and the
wire's T5/T6 then never land even though rawlink and LIVI are running; the
guest log's T6u is unaffected (and is what the smoke check falls back to).
The T3/T4 markers and T5 (when it lands) are unaffected.

### Unmodified (Electron AppImage) vs modified (LIVI-Lite), measured

Same host and command for both, Buildroot 2026.08, kernel 6.18.7, qemu TCG
`-cpu max`, 4 vCPU, 2 GiB, three warm boots each (`tests/smoke.sh`, all
checks pass). "guest" is the kernel/`/proc/uptime` clock, "host" maps the
first printk timestamp onto qemu's process start (T0). qemu numbers are
**relative only** (virtual UDC, no wire, emulated CPU): the board's SD card
and real cores replace them, so they are not a hardware prediction.

| id | milestone | unmodified guest s | modified guest s |
|---|---|---|---|
| T0 | power on / qemu start | - | - |
| T1 | kernel entry | 0.000 | 0.000 |
| T2 | rootfs mounted, init running | 2.74 / 2.95 / 2.77 | 2.81 / 3.06 / 3.04 |
| T3 | gadget bound to the UDC, rawlink running | 12.89 / 13.22 / 12.46 | 13.09 / 13.31 / 13.47 |
| T4 | weston wayland-livi socket exists | 14.56 / 15.12 / 14.48 | 14.55 / 14.66 / 14.66 |
| T5 | first FRAME sent | (15.81) | 15.57 / 15.63 / 15.79 |
| T5/T6 | rawplay wire | mostly not landing (see above) | landing (T5) |
| **T6u** | **LIVI UI presented** | **116.85 / 116.93 / 129.44** | **29.10 / 27.73 / 24.24** |
| | rootfs.squashfs | 310.6 MB | 99.8 MB |
| | disk.img | 444.9 MB | 234.0 MB |

The first boot after a fresh flash is the cold-profile case: the unmodified
image did **not** present the Electron UI within the 900 s scrape window on
its first boot; the modified image presented LIVI-Lite on its first cold
boot too (29.10 s guest, the run-1 row). Warm runs are the other rows.

**Result: `off -> rawplay up` is unchanged (~15 s guest, the same units and
kernel) and `rawplay -> LIVI fully running` drops from ~117 s to ~27 s on
this qemu host, a ~4.4x boot-to-UI improvement** (the rawplay device is
bound at T3, ~13 s; the panel gets LIVI ~12 s after weston is up with
LIVI-Lite, ~102 s with Electron). The remaining LIVI-Lite time is almost
all the nested compositor's software-EGL context under TCG (~11 s between
livi-core start and the compositor display); on real hardware that is a GPU
(or llvmpipe on the board's CPU) and much smaller - for reference the same
native stack starts `livi-core` to Slint UI in **~0.6 s** on an idle x86_64
host with weston pixman.

`systemd-analyze` totals are nearly identical (that is expected - the LIVI
unit is scheduled after `multi-user.target`): unmodified
`multi-user.target reached after 11.31-11.97 s in userspace`, modified
11.04-11.18 s; kernel 3.25-3.49 s vs 3.34-3.40 s. The difference is
entirely inside `livi.service`.

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
* **Native session start.** `livi.service` runs `livi-core` directly; the
  binary starts the compositor, UI, gst-host and helper itself. Compared to
  the Electron build this removes the AppImage/outer-launcher/Chromium cold
  start entirely (the two cold starts cost ~12 s and ~30 s under qemu TCG).
* **Persistent GStreamer registry.** `GST_REGISTRY` on `/data` with
  `GST_REGISTRY_UPDATE=no`: the plugin scan happens only on the first
  boot after a flash, not on every boot.
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
headless+kiosk+pixman + `weston-touch` module stack, that **LIVI-Lite builds
from source with Buildroot's Rust and starts, opens its nested compositor
and Slint UI and renders through the system GStreamer/Mesa stack**,
PulseAudio plus the null sink, and the helper shims (`sudo`, the wifi-ap
unit, the Rust helper's root installs).

**Does not prove**: H618 USB throughput (virtual UDC), any real USB
electrical/timing behaviour, Wi-Fi/BT (UWE5622 driver absent; hwsim is not
a phone), I2S/PCM5102A audio (no driver, no card), real SD-card or CPU
timing, or that the car's i.MX31 rawsplay accepts any of it. The smoke
test's mass-storage check exercises usb-storage and the read-only LUN
inside the guest.

qemu-specific observations worth knowing:

* The Rust nested compositor renders through system EGL. On qemu there is no
  GPU, so that is Mesa **llvmpipe** (`BR2_PACKAGE_MESA3D_LLVM` +
  `GALLIUM_DRIVER_LLVMPIPE`); softpipe is the fallback. The context setup
  under TCG is the ~11 s between livi-core logging `listening` and
  `compositor display`.
* `dummy_hcd` (the virtual host controller in the same guest) has oopsed its
  hrtimer once under sustained bulk traffic; `tests/smoke.sh` retries once
  when the console log says `Kernel panic` and fails on anything else. It is
  a harness flake, not the appliance: the UDC/FunctionFS path itself is
  upstream kernel code exercised identically on the board.
* The virtual USB bulk stream can drop the first MODE message (the guest
  logs `FRAME before MODE, ignored`), which suppresses the wire's T5/T6;
  `qemu_boot.py` then uses T6u from the guest console. This is the same
  qemu/dummy_hcd environment issue, not the appliance: rawlink reports
  `reader ready` and frames go out, and the same binaries on hardware talk
  to the unit's rawplay.
* The first boot after a fresh `disk.img` (an empty `/data`) is the
  cold-profile case: the old Electron image did not reach a presented UI
  within the scrape window, the LIVI-Lite image did (26.09 s guest).
  `GST_REGISTRY` on `/data` makes later boots skip the plugin scan.
* `qemu_boot.py` keeps passing `systemd.setenv=LIVI_INNER_ARGS=--disable-gpu`
  for the unmodified image's Chromium GPU path; LIVI-Lite does not read that
  variable, so it is inert on the modified image and the two runs share one
  command line.

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
6. Wireless: `ip link` should show `wlan0` (firmware load in `dmesg`:
   `loading image [wcnmodem.bin] successfully`), `bluetoothctl list`
   should show `hci0` once `uwe5622-bluetooth.service` binds `/dev/ttyBT`.
   Then join a network (`wpa_supplicant`/`wpa_cli`) and let the helper
   bring up its AP for a phone. If either radio misbehaves, see "Needs a
   decision" item 1.

## Needs a decision

1. **Wi-Fi/BT driver (was blocking wireless AA/CarPlay and Bluetooth
   pairing; now shipped, untested on hardware).** The module is a Unisoc
   UWE5622/AW859A with no mainline driver, so the image builds
   `armbian/uwe5622` out of tree (`package/uwe5622`), enables the SDIO
   bus with a DTS-only kernel patch, ships `wcnmodem.bin`, and attaches
   Bluetooth via `uwe5622-bluetooth.service`. If the hardware bring-up
   shows the BSP driver is unusable, the fallbacks are, cheapest first:
   a. a different board with a mainline Wi-Fi/BT part (AP6256-class) —
      the deployment stays the same;
   b. accept wired-only, which for LIVI means no phone session at all; not
      useful.
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
  the 10 s target. The largest remaining qemu cost is the nested
  compositor's software EGL under TCG; on the board that is the Adreno-class
  GPU (or llvmpipe on the CPU), and the same native stack starts core to
  Slint UI in ~0.6 s on an idle x86_64 host.
* `initcall_debug` parsing is only exercised by debug images; the qemu
  kernel does not use it.
* Wi-Fi/BT and audio are blocked above; the helper's hostapd/BT paths are
  therefore unexercised outside unit-sim's fake gadget.
* The GStreamer registry on `/data` removes the per-boot plugin scan after
  the first boot; a readahead/pre-warm pass over `/opt/livi` is the next
  lever on the board's SD card.
* The `usb.img` in a clean checkout is a stand-in (MBR+FAT with the tracked
  stick base and `homebrew/apps/rawplay.sh`); run `make stick` at the repo
  root before flashing a car-bound card so the unit gets the real launcher,
  HMI assets and the built QNX rawplay.
