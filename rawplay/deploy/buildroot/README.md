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
> image has been run on hardware**: the board boots, the UWE5622 comes up
> (wlan0 + hostapd AP + hci0), rawlink streams to the car and LIVI presents
> its UI, and the H616 **video engine decodes H.264/HEVC in hardware**
> through the cedrus backports below (verified live; see "Hardware video
> decoding"). The Mali-G31 stack (panfrost + Mesa, the upstream H616
> power-domain fix, weston GL, dmabuf zero-copy) ships in the board image
> too; its first hardware outing hit the known pre-fix GPU hang, the fix is
> backported and the GPU path is the one to watch on the next bench run
> (see "The Mali GPU" below). Audio is still blocked ("Needs a decision").
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
                           genimage.cfg, post-image.sh, reference/ (Wi-Fi DT notes),
                           kernel-patches/ (UWE5622 SDIO, HCI quirk, H616 video
                           engine/GPU/PPU 0003-0007, H616 panfrost 0008-0010)
    board/qemu_aarch64_virt/ linux.fragment, post-image.sh
    package/rawlink/       builds ../../../rawplay/out/rawlink (local site method)
    package/weston-touch/  builds weston-touch.so against the image's libweston
    package/livi-lite/     cross-builds pukikiko/LIVI-Lite from source (local site)
    package/livi-appliance/ units, shims, persist/pulse/seed scripts,
                           livi-weston-run (renderer choice), livi-gpu.modules-load
    package/unit-sim/      qemu's car-side rawplay stand-in
    patches/gst1-plugins-bad/ the v4l2codecs system-memory copy fix
                           (BR2_GLOBAL_PATCH_DIR)
  livi-lite-patches/       reference patches for a fresh LIVI-Lite checkout
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
* **Measured on hardware**: the link sustains **18.8 MB/s / 24.4 fps** of
  768 000-byte frames (`scripts/throughput.sh` + `tests/rawsink.py` on a
  bench host, and the same rate from the board's own `rawstream` stats).
  PIO with single 512-byte FIFOs is the limit; there is no DMA path to
  enable. The raw link needs ~23 MB/s for 30 fps, so **30 fps of raw frames
  is out of reach on this board**: the panel can be fed ~24-25 fps, and the
  wire time of one frame (~40 ms) is the latency floor for any full-frame
  update. `rawlink-wait` therefore captures at 24 fps (just under the link)
  so the sender's 2-frame window stays empty: measured 24 fps at 41 ms ack
  latency, against 25 fps at 57 ms when capturing at 30 and queueing one
  frame ahead. More than that (dirty-rect deltas, a DMA-capable UDC, or a
  different wire format) is future work; qemu proves nothing here because
  dummy_hcd is virtual and the wire is free.

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
* Bluetooth is HCI over the same SDIO bus (no UART): `sprdbt_tty`
  presents `/dev/ttyBT0`, `uwe5622-bluetooth.service` binds it with the
  Spreadtrum-patched `hciattach_opi ... sprd` at 1.5 Mbaud (a plain
  `btattach -P h4` registers the controller but leaves the radio deaf to
  inquiry; the `sprd` vendor init is what turns the radio on), and
  `bluetooth.service` is enabled for `bluetoothd`. The BT address is
  stable across reboots: `livi-bt-addr` reuses `/data/livi-config/bt-addr`
  when valid, derives it once from the Wi-Fi MAC (+1, wrap-safe, unicast),
  else from the persisted machine-id; `uwe5622-bluetooth.service` runs
  after `livi-persist.service` so `/data` is mounted first. A flapping
  adapter address orphans every BlueZ pairing (keys are stored under the
  adapter address), so stability is what keeps phones bonded.
  Wi-Fi station + AP come from the same chip: `wpa_supplicant` joins
  networks for bench work while the helper's `hostapd`/`dnsmasq`/`iw`
  path (already in the image) serves the phone-facing AP.
* **Run on hardware**: the board brings up `wlan0` (the firmware loads),
  `hostapd` serves the AP and `hci0` appears; a phone associates, DHCPs and
  the device is pageable. The driver is still a vendor BSP port ("the
  driver is trash" per its own porters): expect flakiness, not Intel
  quality.

### Hardware video decoding: the H616 video engine (cedrus)

The board's video engine decodes the phone's H.264/HEVC **in hardware**. The
SoC's VE is the same Cedar block as the H6's; mainline gained the H616
node/compatible in the "arm64: allwinner: h616: Support Video Engine" series
(Chen-Yu Tsai, Jernej Skrabec), backported for the pinned 6.12 kernel as
board patches:

* `0003-arm64-dts-h616-add-video-engine.patch` — the `video-codec@1c0e000`
  node and the VE SRAM section (one `allwinner,sram` claim, as 6.12's SRAM
  driver reads a single phandle; the node lists the corrected v2 IOMMU
  ports 3/2).
* `0005-soc-sunxi-sram-add-h616-ve-sram.patch` — the H616 VE SRAM section
  descriptor (`sunxi_sram_claim` must know it before cedrus probes).
* `0006-media-cedrus-add-h616-variant.patch` — the cedrus compatible, with
  the H6 variant data (H.264 + HEVC; the H616's VP9 decoder is not
  reverse-engineered upstream yet).
* `0007-pmdomain-sunxi-add-h616-prcm-ppu.patch` — the upstream H6/H616
  PRCM power-domain driver. The VE does not need it; the GPU does, and
  without it panfrost's probe hangs the board (verified).
* `0004-arm64-dts-h616-add-gpu.patch` — the Mali-G31 node (with the
  `prcm_ppu` power domain) and the Zero 2W `mali-supply` wiring. Verified:
  `/dev/dri/renderD128` probes as `mali-g31`.
* `0008..0010-drm-panfrost-*.patch` — Philippe Simons' "drm/panfrost:
  enable G31 on H616" series (v2, dri-devel 2025-04). The H616's GPU
  power domain needs the clocks and reset line sequenced around it; the
  stock 6.12 driver leaves them alone, so the first real GPU work hung the
  whole SoC (verified the hard way: the board wedged solid when weston
  first rendered through panfrost). The series adds GPU_PM_RT, the H616
  compatible that selects it and reorders init to domain → reset →
  clocks. This is what makes the GPU usable at all on this board.

The image turns both on with `BR2_PACKAGE_LIVI_APPLIANCE_GPU`, which
installs `/etc/modules-load.d/livi-gpu.conf` (`sunxi-cedrus`, `panfrost`);
the fragment enables `VIDEO_SUNXI`, the cedrus module, `SUN50I_IOMMU` and
the PPU. GStreamer needs the stateless V4L2 decoders, so `gst1-plugins-bad`
is built with `BR2_PACKAGE_GST1_PLUGINS_BAD_PLUGIN_V4L2CODECS` (the plugin
registers elements only when `/dev/media0` exists — the qemu image carries
it inert).

LIVI's codec probe then reports hardware (verified on the board):

    [core] gst-host codecs {"h264":{"hw":true,"sw":true},"h265":{"hw":true,"sw":true},...}

and the video host picks its sink chain from the compositor's render node
(LIVI-Lite's `presink`, see the reference patch):

    GPU compositor: ... ! v4l2slh264dec ! queue leaky ! waylandsink
    no GPU:         ... ! v4l2slh264dec ! queue leaky !
                    videoconvert n-threads=4 ! video/x-raw,format=BGRx ! waylandsink

The first is the zero-copy pair: the decoder's dmabuf reaches the
GPU-backed compositor without a copy (the GStreamer patch keeps it a dmabuf
because downstream advertised DMABuf caps), the frame is textured on the
Mali and composited into LIVI's surface. The second is the fallback for a
session whose EGL found no GPU, where the direct chain is `not-negotiated`
(verified on the board). The decode is hardware in both cases; the first
still needs its first clean bench run (the pre-fix GPU hang is what the
panfrost backports address).

The decode itself is fast — a 79 s phone capture (1967 frames, 800x480
HEVC) decodes in ~2 s — but the no-GPU fallback showed two CPU-side costs
on hardware, both fixed in this image:

* **videoconvert was scalar.** With the compositor on llvmpipe/pixman the
  frames are converted to the sink's shm format (BGRx) in software, and
  without ORC the defconfigs built GStreamer's plain C path: ~170 ms per
  800x480 frame (~6 fps, verified with `gst-launch-1.0`) with the decoder
  throttled to the same rate. `BR2_PACKAGE_ORC=y` is now in all three
  defconfigs; it is what makes both the converter and the software-decoder
  fallback usable on this CPU.
* **the decoder's dmabufs map uncached.** On this non-coherent SoC the V4L2
  mmap is uncached, so any per-pixel conversion reading straight out of the
  dmabuf pays uncached latency for every read. The global patch
  `patches/gst1-plugins-bad/0001-v4l2codecs-copy-frames-for-system-memory-downstream.patch`
  makes the v4l2 stateless decoders run their existing copy path when
  downstream negotiated plain system memory (a GPU dmabuf sink keeps
  zero-copy): the frame is read back once (~11 ms/frame) and the conversion
  then reads cached memory.

With both, the no-GPU pipeline

    ... ! v4l2slh264dec ! queue leaky ! videoconvert n-threads=4 !
    video/x-raw,format=BGRx ! waylandsink

runs at ~27 ms/frame (~37 fps) against the phone's 25-30 fps stream,
measured on the board with the captured phone stream. The "video
incredibly behind" the first live test showed was this conversion rate, not
the link; the capture used for the measurement came from the reference
patch's own `LIVI_GST_DUMP=<path>` debug hook (it appends the pushed
encoded bytes).

### The Mali GPU (panfrost) and the zero-copy path

* Mesa panfrost is enabled in both board defconfigs. Buildroot's graph
  makes that pull the OpenCL/CLC precompiler (host clang/libclc and LLVM
  built with RTTI); llvmpipe/softpipe stay as the software fallback, and
  only the board images carry it.
* `livi-weston-run` picks `--renderer=gl` as soon as
  `/dev/dri/renderD128` exists (panfrost is in `livi-gpu.modules-load`),
  otherwise pixman. A GL weston advertises the GPU/dmabuf to its clients,
  and `livi-compositor`'s EGL then binds the Mali (its log says
  `compositor display wayland-1 on /dev/dri/renderD128` instead of `-`).
  That sets `LIVI_RENDER_NODE` for the video host, which selects the
  zero-copy `v4l2sl*dec ! waylandsink` chain above.
* State: the pieces are built, the kernel probe is verified and the GPU
  session is run on hardware (weston logs `Using rendering device:
  /dev/dri/renderD128`, `GL_RENDERER: Mali-G31 MC1 (Panfrost)`, and the
  compositor binds it). The zero-copy decoder chain above still needs its
  own bench run; if panfrost turns out unable to import cedrus's NV12
  dmabufs, the fallback is the converter chain
  (`LIVI_WESTON_RENDERER=pixman` forces the software session and the
  readback path).
* **The capture readback was the appliance's real bottleneck, and is fixed
  by two weston patches** (`patches/weston/0001`, `0002`, applied through
  `BR2_GLOBAL_PATCH_DIR`). `weston_capture_v1` reads the output back with
  `glReadPixels`, and on panfrost a tiled render target costs ~80 ms per
  800x480 frame with the GL_BGRA read the compositor picks; that capped
  rawlink's capture at 5-9 fps no matter what the link could carry (measured
  with a capture client on the board; pixman captures the same frame in
  3.7 ms). Patch 0001 gives the headless GL output a **linear dmabuf
  renderbuffer in the compositor's read format** (ARGB8888), so the readback
  is a mapped copy: **15 ms**. A format that does not match the read format
  costs as much again (XRGB8888 + GL_BGRA: 81 ms), and a tiled target with
  the right format is still 82 ms; linear-in-the-read-format is the only
  fast combination found. Patch 0002 skips the asynchronous PBO+fence path
  for outputs that repaint only on capture (`--refresh-rate 0`): the client
  is synchronously waiting for the frame, and the PBO round trip adds
  another 16 ms on panfrost. Result on hardware: the panel now receives
  every frame the link can carry (24 fps at 41 ms ack latency, from the
  default 2-frame window captured at 24 fps).

One upgrade detail:

* `GST_REGISTRY_UPDATE=no` makes the persisted registry sticky, so a decoder
  plugin an upgrade adds would never register. `livi-persist` drops
  `/data/livi-config/gst-registry.aarch64.bin` when `/dev/media0` exists and
  the cached file has no `v4l2slh264dec`; the next livi start rescans and
  writes a fresh cache.

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
  settings); besides 800x480 for the screen and the projection
  (`projectionWidth`/`projectionHeight`, so Android Auto negotiates the
  800x480 tier and the kiosk fullscreen check accepts the window) it carries
  the appliance's network/video defaults:

      "wifiType": "2.4ghz", "wifiChannel": 6, "country": "AU",
      "wirelessAaEnabled": true, "wifiDedicatedInterface": true,
      "carName": "Ford Falcon", "projectionFps": 24

  The phone-facing AP therefore comes up on 2.4 GHz channel 6 under the
  Australian regulatory domain (verified on the board: `[wifi-ap] AP up —
  channel=6`), and the UI setting "FPS" starts at 24 (LIVI's own AA
  handshake still negotiates 30/60 — its protocol's `VideoFrameRate` only
  has those two; 24 is the app-side/CarPlay default). `livi-persist`
  re-applies all of these on every boot, not only on a fresh `/data`, the
  same way it always enforced `wirelessAaEnabled`/`carName`, so an upgrade
  of an already-flashed card moves to the new defaults. `livi.service` also
  sets `LIVI_UI_SIZE=800x480`, which fresh installs size their projection
  defaults from (and `livi-ui` opens at).
* The checkout next to this repo carries two local changes on top of
  LIVI-Lite `main`, both kept as reference patches under
  `livi-lite-patches/`: `0001-reconnect-wake-paired-phones.patch` (the
  wireless first-session wake, see "Android Auto wireless") and
  `0002-convert-hw-decoder-frames-for-a-gpu-less-compositor.patch` (the
  hardware decoder's frames are converted for a GPU-less compositor, see
  "Hardware video decoding"). Buildroot's livi-lite package uses the local
  site method and never applies patches for it, so apply the references by
  hand to a fresh checkout before building.

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
* **Kernel compression: uncompressed `Image`, by measurement on the
  board.** The 38.9 MB Image reads from the boot FAT in 1.65 s at 23 MB/s.
  `Image.zst` is 11.8 MB (0.50 s read) but U-Boot's zstd decoder needs
  2.6-3.1 s for it on this SoC even with `CONFIG_ZSTD_LIB_MINIFY` off, so
  the compressed kernel is ~1.2 s *slower* end to end. LZ4 cannot be used:
  U-Boot's magic check (`04 22`) does not match the kernel's legacy lz4
  frames (`02 21 4c 18`). The lever that remains is trimming the kernel
  itself, which the defconfig does not do yet.
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

### Hardware boot measurements (Orange Pi Zero 2W, real board)

Measured with a timestamped UART capture (U-Boot output; the release image
has no serial shell) plus the board's own `/run/livi-boot.log` markers and
`journalctl -b -o short-monotonic` over the AP, on a cold power-on. Run-to-
run variance is ~±0.4 s.

| milestone | before | after |
|---|---|---|
| SPL banner -> BL31 | 0.47 s | 0.47 s |
| BL31 -> U-Boot banner | 0.30 s | 0.30 s |
| U-Boot banner -> kernel (kernel load included) | 4.59 s | 2.18 s |
| kernel -> systemd (period to PID 1) | 0.86 s | 0.94 s |
| systemd -> multi-user.target | 9.43 s | 4.90 s |
| T3 (gadget bound) | 4.82 s | 4.05 s |
| T4 (weston wayland-livi socket) | 7.25 s | 5.36 s |
| LIVI `[core] UI started` | 9.21 s | 6.25 s |
| **power-on -> LIVI UI started (stopwatch)** | **~15.5 s** | **~9.5 s** |

(The "before" column is the same card before these changes, with the old
U-Boot and the LLVM-linked Mesa: U-Boot scanned two USB buses at 1.15 s
each, started the MUSB/RNDIS gadget, tried to load its environment from the
boot FAT, and read the kernel; userspace then paid 3.2 s of cold weston
start against 65 MB of libLLVM, waited 2.3 s for the UWE5622 firmware to
finish blocking sysinit/D-Bus/LIVI, started livi before weston's socket
existed and lost the start, and restarted it 1.5 s later.)

What produced the after numbers, in order of size:

* **U-Boot trims** (`board/orangepi_zero2w/uboot.fragment`): the board
  defconfig's `CONFIG_DISTRO_DEFAULTS` pulls in USB host (EHCI+OHCI), the
  MUSB/RNDIS gadget, FASTBOOT, networking and an environment load from the
  FAT, none of which the appliance uses. Disabling them (including the
  controller symbols, which `select` `CONFIG_USB` back on) removes ~2.7 s
  from the bootloader. `CONFIG_USE_PREBOOT` still runs the defconfig's
  `usb start` hook, which now just prints "Unknown command 'usb'".
* **Mesa without target LLVM** (`buildroot-patches/0001-mesa3d-panfrost-
  without-llvm.patch` + `scripts/build.sh` applying it): Buildroot's
  panfrost symbol depends on `BR2_PACKAGE_MESA3D_LLVM` and selects the
  OpenCL/rusticl precompiler stack, so the shared libgallium linked
  libLLVM.so (65 MB). Mesa's precomp-compiler mode (`-Dmesa-clc=system
  -Dprecomp-compiler=system`) satisfies panfrost without target LLVM: the
  host-mesa3d precompiler generates the shader blobs at build time, the
  target libgallium no longer links LLVM (verified with `readelf -d`), the
  rootfs drops ~75 MB, and weston/livi-compositor stop faulting it in.
  The qemu image keeps llvmpipe/LLVM and is unaffected.
* **Page-cache warm-up** (`livi-warmup.service` + script): reads the hot
  set (weston, libgallium 20 MB, livi-core/livi-ui 36 MB) in the order the
  boot consumes it, starting in the first systemd transaction (~2.6 s).
  It finishes at ~5.5 s, before livi starts; livi-core-to-UI dropped from
  ~1.45 s to ~0.6-0.75 s.
* **UWE5622 modules in their own unit** (`livi-wifi-modules.service`): the
  ~2.3 s marlin firmware download used to sit in `modules-load.d` and hold
  everything ordered after sysinit (D-Bus, bluetooth, livi) behind it. The
  GPU modules stay in `modules-load.d` (weston reads `/dev/dri` at start);
  wpa_supplicant and the AP unit are ordered after the new unit.
* **`livi-wait-weston` ExecStartPre**: livi-core's nested compositor exits
  when weston's socket is not there yet, so every boot lost a livi start
  (5.3 s) and its restart (8.1 s). The wait script removes both.
* **weston/rawlink no longer order on `livi-persist`**: neither reads
  `/data` (weston: `HOME=/root`, runtime dir `/run/livi`; rawlink: `/opt`,
  `/tmp`, `/sys/kernel/config`), so the ~0.7 s ext4 recovery + config
  seeding no longer delays the display path or the gadget.
* **Uncompressed kernel** (see "Kernel compression" above): zstd in U-Boot
  was slower than the extra SD read on this board; the defconfig is back to
  `BR2_LINUX_KERNEL_IMAGE`, and `boot.cmd` still boots an Image.zst if one
  is present.
* **`livi.service` Unblocked from the 1 s D-Bus start / hci0**: unchanged
  in this round on purpose. The helper makes its one BlueZ connection at
  livi-core start and never retries, so livi waits for `hci0` (5.5 s this
  boot) and for `dbus.service`; those are the remaining gates after
  weston's socket (5.36 s).

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
   should show `hci0` once `uwe5622-bluetooth.service` binds `/dev/ttyBT0`.
   Then join a network (`wpa_supplicant`/`wpa_cli`) and let the helper
   bring up its AP for a phone. If either radio misbehaves, see "Needs a
   decision" item 1.
7. Bluetooth address stability (do this once, it saves a re-pair loop):
   `cat /data/livi-config/bt-addr /run/livi/bt-addr`, `hciconfig hci0 |
   grep "BD Address"` and `bluetoothctl show | grep Address` must all agree,
   and stay identical across a reboot. `ls /var/lib/bluetooth` must show a
   single adapter directory. More than one directory means the address
   flapped and the older directories' pairings are orphaned: remove the
   phone's pairing to the car on both sides (`bluetoothctl remove
   <phone>` + forget "Ford Falcon" on the phone) and pair fresh.

### Bluetooth connection errors ("already connected", "Bad flag 0x2")

* `bluetoothd: device_add_connection() Device XX is already connected` +
  `Device is already marked as connected`: BlueZ already holds an ACL for
  that phone and the kernel reported another one. In practice the phone is
  retrying because the previous attempt never settled a profile: the
  helper's RFCOMM handshake (iAP2/AAP) did not complete, the link key is
  stale (see item 7: an adapter-address flap orphans `/var/lib/bluetooth`
  keys on every boot), or the helper was not yet advertising (it holds BT
  advertising back until its AP is ready). The USB `rawstream` line in the
  same journal is unrelated: video flows over the gadget, Bluetooth is the
  separate phone link. Remedy: check item 7 first, then
  `bluetoothctl info <phone>` (look for `Connected: yes` with no profiles),
  `journalctl -u livi | grep '\[helper'` (helper up?), `bluetoothctl show`
  (`Powered`, `Discoverable`, `Pairable`, `Alias`), and a `btmon` capture
  across the retry. Then remove both-side pairings and pair fresh; a
  phone that was bonded to a flapped address can never come back without
  a fresh bond.
* `Bluetooth: hci0: Bad flag given (0x2) vs supported (0x0)` +
  `set_device_privacy_complete() ... Invalid Parameters`: benign on this
  controller. `0x2` is the LE device-privacy (RPA) flag BlueZ 5.79 sets for
  dual-mode phones; the Marlin/UWE5622 reports `supported 0x0` (no LL
  privacy), so the kernel mgmt layer rejects the flag with `EINVAL`. It
  does not block BR/EDR pairing or the iAP2/AAP RFCOMM session wireless
  CarPlay/Android Auto use; the same pair of lines appears on Intel and
  other controllers without LL privacy. Do not chase it: fix the pairing
  state above instead. `livi-diag` (debug image) dumps the adapter
  address, the persisted/attach/wlan0 addresses and the BlueZ adapter
  dirs for exactly this triage.

### Android Auto wireless: how the session starts, and the two traps

Measured on the board (Pixel 10 Pro XL, btmon + bluetoothd -d + the helper's
own log), the wireless AA chain is:

1. the car pages the phone over **HFP** (`ConnectProfile` on the phone's HFP
   AG UUID - this is what the helper's reconnect loop does for
   `androidauto` targets, and what tapping a phone in the LIVI UI does);
2. the phone's AA app answers the page, opens the car's **AA RFCOMM ch 8**
   SDP record and runs the **WPP bootstrap** (Wi-Fi info is handed over
   here);
3. the phone joins the car's AP and opens **TCP 5277**; TLS, protocol 6.1.
   The helper logs `[aa] phone connected`, `identified instanceId=...`,
   `tls up`, `[core] Android Auto projects over wifi`.

Two traps this exposed:

* **A first-time phone is woken automatically.** Reconnect targets come from
  LIVI's registry (`/data/livi-config/LIVI/devices.json`), which only gains
  the phone after its first successful session - and the phone will not
  start HFP by itself (the helper registers HFP as client-only, so there is
  no HF record in the car's SDP for a phone-side connect). The helper's
  reconnect loop therefore also wakes every **paired** phone that is not in
  the registry yet (`livi-runtime/src/reconnect.rs` in the LIVI-Lite
  checkout): within a second of the pairing connect, or on the next scan
  when the phone is away, it pages the phone's HFP AG (Android Auto) and
  iAP (CarPlay wireless) profiles and the phone's own app starts the
  session; from then on the phone is a registry target and the normal
  round-robin owns it. Everyday users just pair and the projection starts
  (verified on the board). The change lives in the LIVI-Lite checkout and a
  reference copy is kept at
  `buildroot/livi-lite-patches/0001-reconnect-wake-paired-phones.patch`
  (the livi-lite package uses the local/override site method, so Buildroot
  never applies patches for it - apply this by hand to a fresh checkout
  before building). Without that helper patch the first session
  needs a kick from the car side (LIVI UI, or
  `busctl call org.bluez /org/bluez/hci0/dev_XX org.bluez.Device1
  ConnectProfile s 0000111f-0000-1000-8000-00805f9b34fb`). The
  boot-time paging-list send races the helper's socket and can fail once
  (`paging list not sent: ... Connection refused`); the `HelperConnected`
  event re-sends with `again=true`, so the first list always lands.
* **`systemctl restart livi` during an active AA session can deadlock the
  kernel's RFCOMM layer.** The phone's AA RFCOMM ch 8 close races
  `krfcommd`; captured on this board: `krfcommd` stuck in `rfcomm_run`,
  `bluetoothd` stuck in `__lock_sock` (D state, SIGKILL cannot take them).
  BT then stops answering (D-Bus property gets time out, SDP stops
  responding) until a reboot. Stop the session (or just reboot) before
  restarting livi if BT goes zombie-like.

### GStreamer: the "app" plugin is what makes the phone's screen appear

LIVI's media hosts feed their GStreamer pipelines through `appsrc` (e.g.
`appsrc ! h265parse ! avdec_h265 ! videoconvert ! waylandsink`; with the
VPU image the probe picks `v4l2slh264dec`/`v4l2slh265dec` and the chain is
covered in "Hardware video decoding"). The two
board defconfigs originally left `BR2_PACKAGE_GST1_PLUGINS_BASE_PLUGIN_APP`
unset, so on a real session the pipeline failed to parse (`gst-host.log`:
`pipeline parse FAILED: no element "appsrc"`, `create player ... FAILED`)
while the BT/Wi-Fi session itself established normally - the panel then
kept showing LIVI's own "phone connected" UI and the bug looked like an AA
failure. The plugin is enabled in all three defconfigs now; after a build
that includes it the panel shows the projected Android Auto UI (verified
on the board with the plugin bind-mounted in before the image rebuild).
Debugging shortcut: `strings /data/livi-config/gst-registry.aarch64.bin |
grep -c appsrc` - `0` means the plugin is missing; delete the registry
after installing the plugin so it is rescanned (`GST_REGISTRY_UPDATE=no`
otherwise keeps the stale cache, and `livi-persist` now drops a cache that
predates the v4l2 decoders by itself).

## Needs a decision

1. **Wi-Fi/BT driver (was blocking wireless AA/CarPlay and Bluetooth
   pairing; shipped and now run on hardware).** The module is a Unisoc
   UWE5622/AW859A with no mainline driver, so the image builds
   `armbian/uwe5622` out of tree (`package/uwe5622`), enables the SDIO
   bus with a DTS-only kernel patch, ships `wcnmodem.bin`, and attaches
   Bluetooth via `uwe5622-bluetooth.service`. On the board `wlan0`/`hci0`
   come up and the AP serves phones; the driver is still a vendor BSP port,
   so flakiness stays possible. If it turns out unusable in a car, the
   fallbacks are, cheapest first:
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

* The board runs on hardware (Wi-Fi/BT, AP, rawlink, LIVI, hardware video
  decode — see the sections above); audio is the remaining blocked block.
  The GPU/Mesa stack ships and the zero-copy video path is wired to it,
  but its first clean bench run (after the panfrost H616 backports) is
  still outstanding (see "The Mali GPU"). Board boot time is not yet
  measured; the qemu table says nothing about the 10 s target. The largest
  remaining qemu cost is the nested compositor's software EGL under TCG; on
  the board that is llvmpipe on the CPU while no renderer node exists, and
  the same native stack starts core to Slint UI in ~0.6 s on an idle x86_64
  host.
* `initcall_debug` parsing is only exercised by debug images; the qemu
  kernel does not use it.
* Audio is blocked above; the helper's hostapd/BT paths are otherwise live
  on the board now.
* The GStreamer registry on `/data` removes the per-boot plugin scan after
  the first boot; a readahead/pre-warm pass over `/opt/livi` is the next
  lever on the board's SD card.
* The `usb.img` in a clean checkout is a stand-in (MBR+FAT with the tracked
  stick base and `homebrew/apps/rawplay.sh`); run `make stick` at the repo
  root before flashing a car-bound card so the unit gets the real launcher,
  HMI assets and the built QNX rawplay.
