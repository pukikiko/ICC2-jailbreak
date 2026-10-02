# rawplay — raw uncompressed usb video/touch for ICC2!

rawplay sends uncompressed RGB565 frames, in the panel's own size and format, over the
composite livi gadget's vendor interface, with touch/knob/button coming back on the same
wire. The unit end is `rawplay/rawplay.c` + `common/raw-usb.c`; the host end is `rawplay/rawlink.c`.
This document is the reference for the protocol, the device and panel facts, the usb
behaviour and the performance of that path, plus how to build and run it (section 5).

Where the pieces live:

* `rawplay/rawplay.c`, `common/raw-usb.c`, `common/raw-usb.h`, `rawplay/rawplay.sh` — the unit player,
  built by the top-level Makefile (`make out/rawplay`).
* `rawplay/rawlink.c` — the host: the raw sender, and FunctionFS + mass storage on a real
  car, in one binary (`stream`, `gadget`, `run`).
* the ICC2 SDK's `fb.c` / the ICC2 SDK's `input.c` — the shared panel mapping and touch input.
* `rawplay/bench.py` — the benchmark harness; `rawplay/test_rawlink.py` tests the host, and
  `rawplay/test_livi_link.py` the phone deployment's link state machine (`deploy/`).
* `rawplay/deploy/` — the postmarketOS phone appliance: the LIVI + rawlink systemd units,
  and the phone's own `fbkeyboard` console UI, the preferred UI for the phone
  (`rawplay/deploy/README.md`).

Everything claimed here was verified in this repo, on the emulated i.MX31, under
`--icount 2` where performance is concerned.

## 1. The device and the panel

### 1.1 The composite gadget

Two implementations of the same physical device exist:

| | real car | emulator |
|---|---|---|
| gadget | `rawplay/rawlink.c` (Linux configfs) | qemu `hw/usb/dev-livi.c` (`usb-livi`) |
| vendor function | FunctionFS `ffs.livi` | vendor endpoint pair on a chardev |
| mass storage | `mass_storage.0` (configfs) | stock qemu MSD core |

Composition:

* **One configuration**, `bDeviceClass = 0x00` (per-interface classes; QNX's `io-usb` then
  reports each interface to its class driver separately).
* **Interface 0 — vendor**: `0xff/0xff/0xff`, two **bulk** endpoints, `0x83` IN (host→unit)
  and `0x04` OUT (unit→host). Full-speed descriptors use 64-byte max packets, the
  high-speed set uses 512. No isochronous endpoints, no alternate settings.
* **Interface 1 — mass storage**: `08/06/50` (SCSI transparent, bulk-only), bulk `0x81`
  IN / `0x02` OUT, mounted by `devb-umass` as `/fs/usb0` (the homebrew stick).
* IDs `1209:1cc2`, one config, `MaxPower 250`. The vendor interface is found by class,
  not by number, because FunctionFS rewrites placeholder numbers at bind time.

On the unit there is **no kernel driver**: the player links `libusbdi.so.2`, attaches the
vendor interface, opens both bulk pipes and treats them as a byte stream (`common/livi-usb.c`,
and `common/raw-usb.c` for rawplay).

### 1.2 The panel and the IPU

Measured by running `out/gfprobe` on the emulated unit (`qemu/tests/snap.py`,
`upload` + `sh /tmp/gfprobe`), plus the shipped-stack analysis behind it:

* The panel is the **SDC background plane** (IPU IDMAC channel 14), **800x480 RGB565
  (16 bpp)**, stride 1600 bytes. `gfprobe` reads `sdc bg fmt 0x1e0831fa` (bpp code 2 = 16).
  the ICC2 SDK's `fb.c` maps it: it reads the channel-14 parameter words out of the IPU IMA memory
  (`0x53fc0000`) and `mmap_device_memory()`s the physical address at
  `PROT_READ|PROT_WRITE|PROT_NOCACHE`.
* The SDC **foreground plane** (channel 15) is `devg-imx31`'s layer 1 and is owned by the
  reversing-camera pipeline; `devg-imx31` rewrites the IMA/format words from a shadow copy
  every vsync, so poking it from another process does not stick. `gf_layer_query` returns
  error 9 for **every** format on every layer, and layer 2 does not attach at all — there
  is no client-visible API to program the foreground plane.
* **YUV: not reachable.** The IPU's CSC block is loaded (`csc 0x0005000f …`,
  `sdc_com_conf` bit 5 set) but every plane is programmed RGB565 and the client layer path
  advertises only RGB (`imx31_formats = {2,3,4}`). There is no memory-to-memory CSC API;
  the only YUV path is CSI capture (`gf_vcap_*`), not a memory source. **YUV420 planar and
  YUV422 packed are both unusable through the shipped QNX graphics stack**, regardless of
  what the silicon can do.
* **Scaling: not available on this path.** The SDC foreground is a positioned window with
  global alpha and a colour key, not a scaler; there is no client API for it anyway. The
  emulator's IPU model implements no CSC and no scaling either (RGB16/24/32 only,
  `qemu/qemu/hw/display/imx31_ipu.c`).
* **Zero-copy**: the panel buffer is `mmap_device_memory`'d by every player, with a known
  physical address until the HMI resumes. `usbd_alloc()` gives DMA-safe buffers for USB;
  whether USB can DMA straight into the panel mapping is section 2.3.

### 1.3 Bandwidth at 800x480

Raw RGB565, the native scanout format, is 768 000 bytes/frame:

| fps | bytes/s | bitrate |
|---|---|---|
| 15 | 11.52 MB/s | 92.2 Mbit/s |
| 30 | 23.04 MB/s | 184.3 Mbit/s |
| 60 | 46.08 MB/s | 368.6 Mbit/s |

* Full speed USB (stock `force_fs`): ~1 MB/s bulk → **~1.4 fps of raw 16 bpp. Raw video
  cannot work on a stock unit over the full-speed port.**
* High speed USB: ~35–45 MB/s of bulk in practice → 30 fps comfortable, 60 fps near the
  wire limit. The jailbroken boot path already brings the port up without `force_fs`
  (`docs/homebrew.md`, `usb/homebrew/apps/usbhs.sh`), and the emulator is high speed
  only and has no wire cost at all.
* YUV422 packed would be the same 16 bpp; YUV420 planar would be 12 bpp but is *not
  reachable* (section 1.2). RGB565 written straight to the scanout buffer is the optimum:
  byte-identical to what the panel displays, so the unit does no conversion.
* At 30 fps the emulator's bottleneck is the guest CPU (what `--icount 2` models), not the
  link. In the raw path the guest CPU never touches a pixel; its per-frame work is URB
  handling and one ack.

## 2. Design

### 2.1 Gadget: the shape and why

The kernel-visible gadget is deliberately plain: one configuration, FunctionFS vendor
interface (class `0xff`, two bulk endpoints) + mass storage function, same IDs, same
enumeration as section 1.1. Justification:

* bulk FunctionFS is the lowest-latency path this QNX stack can use (the unit only speaks
  `libusbdi` bulk); a custom kernel gadget function would add a kernel module on the host
  and change nothing about the UDC/DMA path the bytes take.
* keeping mass storage on the same configuration is a hard requirement and is proven:
  `devb-umass` mounts `/fs/usb0` while the vendor interface streams.
* FunctionFS descriptor rewriting means the unit finds the interface by class, so the
  protocol on the wire can change without any enumeration change.

`rawplay/rawlink.c`'s `gadget` role is that entry point: it writes the same configfs setup
and descriptor bytes the device has always had (`--name`, `--ffs-mount` and `--sock` let
several instances run side by side). Nothing in the kernel or qemu changes; qemu's
`usb-livi` pipes bytes and is protocol-agnostic.

### 2.2 Wire protocol: `LR` raw framing + the existing `LI` upstream

Downstream uses its own magic so a receiver can resync mid-stream and unrelated traffic on
a shared link can be discarded without parsing:

```text
offset  size  field
0       2     magic 'L','R'
2       1     type        1 = MODE, 2 = FRAME
3       1     flags       reserved, 0
4       4     len         payload bytes after the header (u32 LE)
8       4     seq         frame sequence (u32 LE)
12      4     ts_ms       host monotonic milliseconds (u32 LE)
16      ...   payload     MODE: starts here, then zero pad to 512. FRAME: starts at 512
                          (the header is zero padded to one max packet, see below)
```

* **MODE** payload (16 B): `width, height, stride, format` (all u32 LE; format 1 =
  RGB565LE, rows top-to-bottom, no padding). Sent at start and answered to every reader
  heartbeat, so a reader that starts or resyncs mid-stream gets the current geometry. The
  receiver validates `width*2 == stride` and `stride*height` fits its buffer. The 16-byte
  message is padded to 512 so the *next* message starts on a maxpacket boundary.
* **FRAME** payload: exactly `stride*height` bytes (768 000 for 800x480). This is the
  scanout image, byte for byte; the receiver writes it straight into the display mapping.
* **messages are maxpacket-aligned**: MODE is padded to 512 bytes total, and a FRAME's
  16-byte header is followed by 496 zero bytes before the payload, so its payload starts at
  offset 512 and the whole message is `512 + stride*height` — a whole number of 512-byte
  packets. The receiver reads every message as whole maxpackets and its ack does not depend
  on the first bytes of the *next* message any more. With the old layouts a frame ended
  mid-packet (or a 32-byte MODE left the next read straddling two messages), the last bytes
  went through a 512-byte read that could only complete once the next frame started
  arriving — which pinned every ack to the next frame (a frame period of latency at 60 fps)
  and deadlocked the link when the host's window held only one frame. Host and receiver
  must be the same build: this is a wire change.
* seq/ts let the receiver count drops and lets the host compute latency without a shared
  clock.

Upstream reuses the fixed `LI` messages (touch 8 B, knob 4 B, button 5 B, ready 3 B) and
adds:

```text
'L','I', 8, seq(u32 LE), status(u8)     total 8 bytes
status bit 0 = displayed, bit 1 = stale-dropped
```

This single message carries both flow-control credit (any consumed frame) and the latency
sample (displayed frames only). At 30 fps it costs 240 B/s.

### 2.3 Unit receiver: `rawplay`

* **Display**: `fb_open()` (unchanged, the ICC2 SDK's `fb.c`) maps the SDC background plane at
  800x480/stride 1600/RGB565. The receiver pre-clears it to the app background and never
  converts or scales a pixel.
* **Video path, primary (zero-copy)**: the payload bulk URBs are pointed **directly at the
  mapped panel buffer** (`fb.pixels + offset`), so the USB controller's DMA writes the new
  frame into the scanout buffer; the unit CPU copies nothing. The padded `LR` header (512 B)
  is read into a small staging buffer (it cannot live in the scanout image). Frames are
  streamed in URBs up to 256 KB (the `--urb` default; up to 1 MB), 4 in flight; the
  maxpacket padding means a 768 000-byte frame is exactly three of them and no sub-packet
  tail URB. This is what the emulator runs (`--direct`),
  but **on a real unit the first frame lands and then the bulk-in pipe stops** — the
  host's reads never complete and the gadget's write blocks — so staged is the real-car
  default; see section 3.
* **Video path, fallback (one copy)**: read into a `usbd_alloc` staging buffer and
  `memcpy()` into the panel mapping. This is announced loudly on the console and in
  `--stats`; it is not a silent fallback. It is still decode-free and conversion-free, and
  it is the default (`--stage` is now implicit; `--direct` opts into zero-copy).
* **Newest-wins**: the host keeps at most two unacked frames in flight, so no more than
  that can queue ahead; a frame that is already superseded (another complete header is
  pending by the time the receiver is ready) is consumed and acked as `stale-dropped`
  without display. No queue is ever built in userspace.
* **Touch**: the ICC2 SDK's `input.c` unchanged — `/dev/devi/touch0` and `/dev/ipc/0` feed the same
  lock-free rings; the main loop sends the same `LI` messages. No evdev/X roundtrip exists
  anywhere on this path and none is added. The input thread is woken by the producer's
  post (`input_wait_event`), not by a poll interval: a sample leaves within the loop's own
  few instructions of arriving. It forwards at most one touch sample per pass: press/move
  samples coalesce to the newest position and a release is sent only for a press the host
  was told about. `--poll-us` only sets a lost-wakeup timeout. The driver reports a sample
  every millisecond while a finger is down and replays the samples queued before the
  player opened as one burst with the next touch, so without that the replayed history (the
  launch tap, the previous run's exit gesture) reaches the host as a flood of moves and
  phantom clicks.
* **Timing/acks**: each consumed frame sends `LI type 8` with the displayed/stale bit; the
  host's latency sample is send-time → displayed-ack.
* **Heartbeat and mode refresh**: a `ready` heartbeat goes out every second, and the host
  answers it with the MODE message, so a reader that starts or resyncs mid-stream is told
  the current geometry without a separate handshake.
* **Outbound queue, not blocking writes**: no thread writes the bulk-out pipe directly.
  Touch/knob/button and acks/ready are queued and one writer thread drains input first,
  then the newest ack, then the ready flag (`rawplay/rawplay.c` writer_thread). The writer
  sleeps on an event posted by whoever queues and wakes immediately, so an ack goes out the
  moment its frame lands (the host's latency sample is send-to-ack) instead of after a
  fixed tick. This keeps touch responsive at a low frame rate: a touch never waits behind
  an in-flight ack, and an ack never stalls the loop that has to keep reading frames. A
  full queue drops the touch or the obsolete credit, never blocks.
* **Exit/UI**: during streaming the panel is the video; a reserved top-left 96x36 touch
  region exits on a brief held press (a CPU-written button on top would dirty cache lines
  the IPU/DMA share). The hold matters: the touch driver hands a fresh reader the samples
  queued before it opened, delivered as a burst with the next touch, so without it the
  first touch of every run after the first replays the previous run's exit gesture and
  quits it. The same replay is why the input thread only forwards the newest sample of a
  pass. Status/errors print to the console and are drawn on the panel only while no
  frames are flowing. `rawplay.sh` stops/restarts the HMI around the run.
* **Climate keys with `--hmi`**: the hvac state changes on the v850's own side when a
  fascia key is pressed, but the only place it is shown is the HMI's climate bar, and the
  HMI is stopped behind the video. The player watches the same button bitmap the HMI's
  `buttons` service does (ipc channel 6); on one of the thirteen climate keys it stops
  reading (a stale ack makes the host drop what its two-frame window holds), resumes the
  HMI, and stops it again 2 s after the last key, so the bar is up while the panel is
  being used and the video comes back on its own. A held key holds the window open. The
  bar is raised by a `buttons` event, so the player brings the frozen `buttons` service
  back for the window the way the session resume does: SIGKILL it and let `ham` start a
  clean one, never a SIGCONT (which would replay every panel event queued during the video
  as one burst), then SIGSTOP the fresh process when the window closes, before the HMI
  stops, so nothing it generates queues. The main loop owns every stop/resume so no
  in-flight bulk-in can land in the panel buffer while the HMI draws. `rawplay.sh` always
  passes the flag: it or its caller has stopped the HMI by then.
* **The button reader is self-healing**: `/dev/ipc/0` is a monitor connection that can go
  quiet or fail after an HMI stop/resume, and the panel then stays deaf for the rest of the
  run while the `buttons` service and the HMI keep working. the panel reader in the ICC2 SDK's
  `input.c` reopens the monitor after a read error or 5 s without a frame (the climate window pauses
  the player for about 2.5 s, so that is not a live link) and keeps the previous bitmap, so
  the first frame after the gap still reports what changed.
* **Whoever stops the HMI stops `buttons` with it**: with the HMI frozen every panel event
  `buttons` sends blocks in its queue and the whole backlog replays as one burst when the
  HMI resumes (volume jumps, menus open by themselves). `rawplay.sh` and the launcher/menu
  SIGSTOP `buttons` for the session and SIGKILL the frozen process on the way out, so its
  `ham` guard starts a clean one and the queued frames die with the old connection. The
  resumed HMI only ever sees the current panel state. The one exception is a climate
  window: `buttons` is restarted for it (SIGKILL and a fresh `ham` process, so the queued
  backlog dies with the old connection, not a SIGCONT that would replay it) and SIGSTOPped
  again when it closes, still under the session's pause marker, so the session resume kills
  it as usual.

### 2.4 Host: `rawplay/rawlink`

`rawplay/rawlink.c` is one binary with three roles — `stream` (the sender), `gadget` (the
configfs/FunctionFS bridge) and `run` (both, the real-car appliance mode). It speaks the
wire protocol byte for byte (MODE/FRAME fields and framing, `LI` parsing, window/ready/ack
handling).

* Source set: `--wayland` (the live headless weston session, the appliance source),
  `--test` (moving red box + static blue box at `(20,380)`), `--motion`, `--file`. The
  synthetic sources are ffmpeg, which outputs `-pix_fmt rgb565le -f rawvideo -` at the
  requested fps; frames are read whole (768 000 B). `--wayland` needs no ffmpeg, no X
  server and no window capture: see the capture note below.
* **Wayland capture**: `--wayland NAME` connects to weston's socket and creates a
  `weston_capture_v1` source for the output (the protocol's framebuffer source). The
  compositor captures into a client `wl_shm` buffer on every repaint, and a capture thread
  converts the XRGB8888/ARGB8888 pixels to the panel's RGB565LE and pushes whole frames
  into the same pipe the ffmpeg sources use, so the sender loop is unchanged. The weston
  output is run with `--refresh-rate 0`, which makes it repaint *only* when a capture is
  requested; the thread paces captures at `--fps`, so no compositor cpu is spent on frames
  the unit cannot take. `weston_capture_v1` is a privileged protocol: weston refuses every
  shot with `unauthorized` unless an authority allows it, and the appliance's weston runs
  with `--debug` (its allow-all screenshot authority; a single-user kiosk). The connection
  is retried and the source rebuilt if weston restarts, and a size other than the wire
  geometry, an unsupported format or a disappearing output all log and retry visibly.
  A GL-rendered session can deliver bottom-up frames on drivers without
  `GL_ANGLE_pack_reverse_row_order` (weston's async capture path flips based on that
  extension, not on the real y orientation; NVIDIA is the common case), so `--flip`
  reverses the rows in the conversion. Pixman sessions never need it.
  The repo carries the vendored `weston-output-capture.xml` (`rawplay/protocol/`) and
  generates the client glue with `wayland-scanner` at build time.
* Sends `MODE` at start (and again whenever a reader heartbeat arrives), then one `FRAME`
  per captured frame with `--fps` pacing; a window of `--window 2` frames is kept unacked
  (keeps one frame on the wire while one displays). Open loop `--window 0` is available
  for the drop-stale test.
* Parses `LI` upstream: touch and the panel keys are injected into the host session
  through the `weston-touch` module's unix socket, so they are real `wl_touch`/wayland
  keyboard events with no X server and no XTest (`weston-touch.c` 'd/m/u/c' and 'k').
  Injection is enabled even when the socket is not there yet (the appliance boots rawlink
  before weston) and the connect is retried on use, so a touch that arrives after weston
  comes up still lands. Knob messages are consumed and left alone: livi has no default
  binding for them. The panel buttons that livi has default key bindings for are tapped on
  the press edge as **linux evdev key codes**, from the ipc channel 6 bit the unit sends
  (docs/v850-ipc-protocol.md, `iccbuttons/iccbuttons.c`): **back / home (25) → evdev 14
  `Backspace` back**, **menu (28) → evdev 35 `H` home**, **seek down (29) → evdev 48 `B`
  previous**, **seek up (30) → evdev 49 `N` next**, **swc phone (46) → evdev 47 `V` voice
  assistant**, **swc seek (48) → evdev 49 `N` next**. The press edge only: a release is
  the other half of the same tap, and tapping it again would fire the livi action twice.
  Buttons with no binding are ignored, and nothing in the unit's own handling changes (it
  sees the same bitmap it always has). Type 8 feeds the window and the latency statistics.
  `--events` writes a machine-readable log for the benchmark harness: `touch`, `button`
  (with the `key N` an evdev-coded press taps), and `drawn` lines. A stale
  `/tmp/livi-touch.sock` (weston gone, file left behind) is reconnected on use rather than
  disabling injection.
* **The socket is written non-blocking, newest-wins.** A slow guest must never sit the
  read loop in a 768 KB `sendall` while acks and touches wait behind it: at most the
  window's frames are buffered, and a frame captured while that is full is dropped at the
  source (counted in the stats as `dropped`). The panel always gets the newest picture and
  input stays immediate regardless of frame rate. `--window 0` is bounded the same way.
* **`run` mode moves a whole frame per pipe fill and a quarter of one per FunctionFS
  write.** FunctionFS allows exactly one bulk-in request per endpoint and a synchronous
  `write()` blocks until it completes, so the per-write userspace wakeup lands between
  every chunk. The video pipe is 1 MB (a whole 768 KB frame fits, so the sender keeps the
  endpoint fed while it captures the next frame) and the bridge reads/writes in 256 KB
  chunks (four remotes instead of twelve per frame). This is what keeps a phone that idles
  in deep CPU states from painting the panel line by line; see section 3.
* A fresh reader's first heartbeat resets the latency window and discards acks for frames
  that were queued before the reader existed, so a one or two second startup wait no
  longer shows up forever as `latency ... max` in `--stats`.
* A run of `LI` touch messages in one read is coalesced to its newest sample before
  injection (a different message or the end of the read flushes it): a replayed queue or
  the driver's once-a-millisecond stream can no longer move the host pointer through stale
  positions and presses. `rawplay/rawlink.c` (`touch_flush`) implements that policy and
  `rawplay/test_rawlink.py` checks it.
* **The capture itself must not buffer.** the wayland source always has at most one frame
  in the in-flight capture task and one in the pipe: the thread waits for `complete`,
  converts and writes it, and only then asks for the next repaint, so there is no queue to
  build lag. The weston session is run with `--refresh-rate 0` (repaint only on capture),
  which is what makes the capture the sole clock: no compositor work happens between
  frames, and `--fps` paces the whole chain. The synthetic ffmpeg sources are paced by
  ffmpeg (`-re`, `-framerate`/`-r`) and default to 30 fps.
* No encoding, no bitrate, no VBV, no keyframes: the host writes each captured frame once.
* **A host that does not fall over**: a lost unix socket is retried, a dead ffmpeg pipe is
  respawned, malformed `LI` messages are skipped, and the gadget is rebound if functionfs
  unbinds, so the only clean exits are `--seconds` and a signal.

### 2.5 What is intentionally *not* hardware-accelerated, and why

* **YUV→RGB conversion and scaling on the IPU**: not reachable through the shipped QNX
  stack (section 1.2). The host does the conversion (it is free there — the source is
  already RGB) and sends exactly the panel's format at exactly its size; the unit's CPU is
  spared entirely, which is the optimisation that matters under `--icount 2`.
* **Isochronous endpoints**: not used. Bulk with 2 frames of window is simpler, has no
  reserved bandwidth, and the ACK-driven window already bounds latency.
* **Double-buffered scanout / vsync-synchronised flips**: unavailable: the SDC foreground
  overlay cannot be programmed and the background plane has a single driver-owned buffer,
  so the frame is written into the visible buffer in place (progressive display, possible
  tearing at frame boundaries). A double-buffered extension is future work (section 6).

## 3. USB transport notes (emulator and hardware)

These are the transport facts that shaped rawplay; they apply to any application on the
gadget's vendor interface.

* **qemu's EHCI does not retire the remaining qTDs of a multi-qTD bulk-IN URB** when the
  device returns a short packet, so the guest reads in **16 KB URBs (one qTD)**, 4 in
  flight. On real hardware this bug does not exist, but 16 KB URBs are harmless there.
* **qemu's `usb-livi` buffers only 64 KB** toward the device and has no pacing; the unix
  socket backpressures the host. A 768 KB raw frame must therefore be **streamed in
  chunks**, not delivered atomically. The ring is now 1 MB (`LIVI_BUF`) and
  `CHR_READ_BUF_LEN` is 64 KB, otherwise a frame is delivered through the qemu main loop a
  sliver at a time while the guest waits.
* **`usb-livi` has a `chunked` property**: when set, the vendor bulk-in endpoint naks
  until a whole request fits in its ring instead of returning a short packet, which is
  what functionfs does on real hardware. `hmi.py --livi-usb` sets it, fresh boot and hotplug.
  Without it the zero-copy path sees short
  completions, and rawplay announces the fallback and switches to the staging ring after
  the second short completion rather than compacting a whole frame.
* **Every bulk-in request is a whole number of max packets** (512 high speed). The device
  sends maxpacket-sized packets and does not know the host's request size; a request that
  is not a multiple of the maxpacket makes the host drop the tail of the last packet and
  the ehci side error the transfer (`USBD_STATUS_CMP_ERR`/IO on every read). qemu's
  `chunked=on` model honours request sizes, so this only shows up on hardware. The
  maxpacket-padded FRAME header (section 2.2) means the receiver never needs a sub-packet
  tail read for a frame at all; a short read that does carry a tail keeps the overshoot in
  `rxbuf` for the next read.
* **A bulk-in error is recovered, not fatal.** The QNX stack can hand back a completion
  with `USBD_STATUS_CMP_ERR` under load (seen at 120 fps in the emulator, and as the
  every-read error on a real car). The bytes of that transfer are gone, so rawplay drops
  the frame it was reading (acks it stale, so the host's window moves on) and scans for
  the next `LR` header. The urbs that were still in flight belong to the abandoned frame,
  so they are tagged: the next read consumes their completions without copying
  (`abandon_read`/`rx_drop`) instead of waiting for them or letting a late completion land
  in the wrong place. Recovery is therefore immediate; a halted pipe is reset in the
  completion path once nothing is in flight. Before this, the read waited on bytes that had
  already been consumed and the link died with "usb read failed" once the quiet timeout
  expired.
* **Do not abort urbs on this stack.** A read timeout followed by `usbd_abort_pipe()` can
  segfault the completion thread (exit 139) when the stack never hands the cancelled urb
  back. The transport therefore never aborts: it waits for the gadget or for the removal
  callback (`dead`). This also means no startup flush is needed; the host window bounds
  the pre-ack queue to two frames.
* **URB size is the guest-CPU lever.** 16 KB urbs cost a guest round trip each: 47
  urbs/frame measured ~44 ms/frame of guest time. 256 KB urbs (3 per frame, last sized to
  the remainder) give **~4.1 ms/frame** in direct mode at host speed. `--urb` sets the
  size (default 262144, min 512, max 1048576 — the staging ring slots stay 256 KB, so a
  larger urb only changes how the direct path splits a frame; one whole frame per urb was
  no faster than three 256 KB ones, and 16384 made no difference to the real-car direct
  stall).
* **The zero-copy path stalls on real hardware** after the first frame (the host's bulk-in
  reads stop completing while the gadget's write blocks); `--urb 16384` stalls the same
  way, so it is not the urb size. Staged mode is the real-car default. `--direct` stays
  for the emulator and for chasing the hardware issue.
* On a real car the gadget is unchanged: functionfs already delivers request-sized
  transfers because the bridge writes whole chunks, and the wire, not qemu's ring, is the
  limit.
* **FunctionFS is stop-and-wait with one request per endpoint.** `f_fs.c`'s synchronous
  `write()` path queues the endpoint's single `usb_request` and waits for its completion
  before the writer can queue the next one; the gap is USB transfer + completion IRQ +
  userspace wakeup. At 64 KB a chunk is ~1.4 ms of high-speed wire time, so on a host that
  leaves deep CPU idle slowly (a phone with the screen off, `cpu-power-collapse` at a
  270 µs exit latency) the gap dominates and the link crawls; the panel paints a frame
  line by line. The phone deployment therefore holds `/dev/cpu_dma_latency` at 0 while a
  rawplay client is connected (`livi-link-monitor`, which releases it again when the link
  goes idle; see `deploy/README.md`) and the bridge moves 256 KB per write over a 1 MB
  pipe. If a
  host still caps out, the next step is native AIO (`io_submit`) on ep1, which lets
  several requests be outstanding at once — synchronous writes cannot.

## 4. Performance

`rawplay/bench.py` is the only source for numbers. It runs the raw pipeline end to end over
the gadget, checks the test pattern's screendump pixels and the mass-storage mount, uses
QMP clicks for touch, and can record the panel to a video. `--climate` (snapshot resume,
not `--boot`) presses a climate key while the video runs and checks the HMI window: one
open/close pair on the console for the HMI and one for the restarted/stopped `buttons`
service (the climate bar is raised by a buttons event), a displayed-frame ack gap of about
two seconds, the panel changing to the HMI and back, and no window for a non-climate key.
`--direct` runs the zero-copy path the table above was measured on; without it the
benchmark runs the staged default, which is what a real unit uses, so compare builds under
the same flags. A
host-speed (no icount) mode exists for iteration; the `--icount 2` numbers are the ones
that count.

Why `--icount 2`: by default qemu runs the cpu as fast as the host allows, so the emulator
is faster than the real 532 MHz chip and the fps counter is optimistic. `--icount N` ties
the guest clock to instructions executed at 2^N ns each, which makes the guest perceive a
fixed speed no matter the host:

* N=1 is about 500 mips, N=2 about 250 (closest to the real arm11's effective throughput),
  N=3 about 125.
* it is deterministic and the whole emulator then runs slower than real time.
* it is still optimistic: qemu models neither the cache nor the 133 MHz sdram, and those
  stalls dominate real arm11 performance. Treat it as an upper bound, not a measurement.

Representative numbers, `--boot --icount 2`, one fresh boot per run, motion source at
60 fps, 10 guest seconds, six QMP clicks for touch, no other emulator running. "before"
is the same harness on the build before the 2026-10 work (host and unit from that tree);
"after" is the current one:

| path | build | wall frames/s | rawplay guest fps (source) | ms/frame | latency avg / p95 / max | touch round-trip |
|---|---|---|---|---|---|---|
| `--direct` | before | 58.9 | 97.9 (60.0) | 9.9 | 20.9 / 24.1 / 41.7 ms | 66.6 ms |
| `--direct` | after | 59.0 | 92.4 (60.0) | 1.5 | 4.0 / 5.2 / 26.7 ms | 52.5 ms |
| `--stage` | before | 58.1 | 101.2 (60.0) | 9.7 | 21.3 / 25.1 / 59.9 ms | 70.3 ms |
| `--stage` | after | 58.4 | 90.5 (60.0) | 3.6 | 8.4 / 10.9 / 49.8 ms | 52.6 ms |

The direct rows are the emulator's zero-copy path; the staged rows are what a real unit
runs (section 3). The latency column is the host's send-to-ack sample (the `drawn`
events): the ack leaves the unit the moment the frame lands, so it is the time the bytes
spend on the wire plus the read, not a poll or a display interval. The after build's ack
no longer waits for the first bytes of the next frame — every `LR` message is padded to
whole 512-byte packets (section 2.2) — which is what took the 60 fps latency from about a
frame period to a few milliseconds. The touch round trip includes the QMP injection, so
the unit-side time is smaller than the number printed.

At `--icount 3` (half the guest clock again) the same direct case shows where the old
build starts to pay for the per-ack wait and its guest work:

| build | wall frames/s | rawplay guest fps | ms/frame | latency avg / p95 / max | touch round-trip |
|---|---|---|---|---|---|
| before | 37.1 | 51.2 | 18.9 | 19.2 / 21.1 / 39.9 ms | 37.5 ms |
| after | 38.9 | 53.6 | 1.8 | 3.2 / 4.6 / 27.4 ms | 32.2 ms |

Latency stays at about a frame period for the before build while the after build stays at
a few milliseconds, and at this clock the before build's ~19 ms/frame of guest work starts
to bind the wall rate, which is where the after build's lower per-frame cost turns into
frames (+4.9%). Both `--icount 3` runs failed the mass-storage mount check at boot (the
stick enumeration times out at that clock; the vendor interface, the pipeline and the
checks that matter here are unaffected and identical on both builds).

The host read loop never blocks on the video socket, so even when a source is ahead of the
panel only the newest frame is sent and a touch is injected the moment it arrives.
`--window 2` (plus the one frame being flushed) bounds what the panel can lag by, and a
fresh reader resets the latency window. `--window 1` was tried and dropped: with one frame
unacked the reader's next bulk-in urb sits unanswered across the whole ack round trip, and
under `--icount 2` the emulator's endpoint errors that long naking transfer (the reader
recovers and resyncs, but every frame pays for it). The guest's own fps number moves with
the icount clock and host load; rawplay's stats print the source cadence next to it
(`fps (source N)`) so it is clear the panel is showing every frame the host sent. A frame
rate that still looks low is the source's, not the pipeline's: 768 KB/frame at 15 fps is
92 Mbit/s.

### The wayland-capture change (2026-10)

The sender's capture source changed from ffmpeg `x11grab` on an Xvnc-backed weston to the
in-process `weston_capture_v1` client on a headless weston. Nothing on the unit changed
(`rawplay.c` was not touched), so the two checks are the unit-side `bench.py` A/B and a
host-only capture A/B.

`bench.py --boot --icount 2 --seconds 10 --source motion --fps 60 --direct`, one fresh
boot per build, old = the pre-change rawlink, new = this one:

| build | frames | guest fps | ms/frame | latency avg/p95/max | touch |
|---|---|---|---|---|---|
| old (x11grab) | 1061 | 106.1 | 1.6 | 4.9 / 6.9 / 32.1 ms | 59.9 ms |
| new (wayland) | 1103 | 110.2 | 1.5 | 5.1 / 7.7 / 37.1 ms | 66.7 ms |

The differences are run-to-run host noise (the README table's `after` row, measured on the
same harness in an earlier session, shows the same spread); the per-frame guest work is
identical (1.5 vs 1.6 ms).

The host capture A/B fixes the content (an animating `weston-simple-shm` in a kiosk
session) and the sink (a fake unit that acks every frame), and measures a 10 s / 8 s run
at 30 / 60 fps:

| chain | source fps held | latency avg/p95 | rawlink cpu | weston cpu | xvnc cpu |
|---|---|---|---|---|---|
| old: Xvnc + weston x11 + x11grab | 27.9 / 57.7 | 0.45 / 0.85 ms | 0.41 / 0.61 s | 0.20 / 0.17 s | 0.11 / 0.10 s |
| new: headless weston + capture | 30.0 / 60.0 | 0.40 / 1.05 ms | 0.37 / 0.59 s | 0.14 / 0.20 s | - |

The new path holds the requested cadence (x11grab tops out just under it on this host),
drops the X server's cpu entirely, and has the same or slightly lower total chain cost;
latency is unchanged within the ack loop's resolution.

The emulator's numbers are an upper bound: it is high speed with no wire cost, while a
stock unit's `force_fs` port cannot carry 768 KB/frame at all, and real hardware has
cache/sdram stalls qemu does not model.

## 5. Build, run and test

### Build

* Unit code: `make out/rawplay`.
* C host: `make -C rawplay rawlink` (`rawplay/out/rawlink`), then
  `python3 rawplay/test_rawlink.py`
  (descriptors against the frozen device bytes, protocol bytes vs ffmpeg, hostile `LI`,
  reconnects, a dead ffmpeg, SIGTERM, the gadget bridge `rawplay/test_gadget.c` against
  fifos, and the `weston_capture_v1` path against a real headless weston when it is
  installed). Needs wayland-client and `wayland-scanner` at build time (`wayland-dev`);
  there is no X11 dependency anywhere. `make -C rawplay` also builds the
  `weston-touch.so` module the session loads (weston-devel).
* Phone deployment state machine: `make -C rawplay livi-cmd` then
  `python3 rawplay/test_livi_link.py`. It drives `deploy/livi-link-monitor` with a fake
  journal, `/sys`, `/dev`, power supply, rfkill, systemctl and helper socket and checks
  the charger/radio transitions (unplugged/idle/connected), the latency fd hold, the
  `livi.service` stop on unplug / start once the radios are confirmed (including a
  failed reload and a livi started behind the monitor), that the helper socket is never
  touched (wireless AA is parked on in LIVI's config) and `livi-cmd` itself.
* qemu is prebuilt at `qemu/qemu/build/qemu-system-arm`; a rebuild is only needed after
  touching `qemu/qemu`.
* Real-car gadget runs need the homebrew stick image: `python3 mkusb.py` ->
  `usb.img` (uses `guestfish` if present, falls back to `mtools`).

### Emulator

The display end is LIVI in the virtual wayland session (`rawplay/livi.sh`): weston's
headless backend, kiosk shell, repaint-on-capture, no X server. qemu's `usb-livi` uses
request-sized bulk transfers (`chunked=on`), which is what makes the zero-copy path work.
The default renderer is pixman (what the phone runs, and what LIVI 8.3.0 needs); LIVI
9.0.0 forces its inner app to Wayland and its GPU process needs a dmabuf-capable parent,
so on a GPU host run it as `LIVI_WESTON_RENDERER=gl ./livi.sh start` and pass rawlink
`--flip` (a pixman session makes Electron die with `create_immed ... invalid wl_buffer`
and the panel stays black; NVIDIA's GL capture is bottom-up):

    rawplay/livi.sh
    make -C rawplay rawlink
    cd qemu && ./hmi.py --livi-usb              # --icount 2: fresh boot, no --loadvm
    # host, in another shell (hmi.py prints the exact socket path; $XDG_RUNTIME_DIR is
    # where livi.sh put wayland-livi):
    rawplay/out/rawlink stream qemu/usb.<pid>.sock --usb \
        --wayland "$XDG_RUNTIME_DIR/wayland-livi" --stats
    # at the car prompt (start the sender first, rawplay waits 15 s for video):
    upload ../out/rawplay ../rawplay/rawplay.sh
    sh /tmp/rawplay.sh 0 --stats --direct

Raw pipeline alone (fast iteration on the `started` snapshot; the usb stack may need a
restart, which `bench.py` does):

    cd qemu && python3 ../rawplay/bench.py --seconds 8 --source test
    # or under the arm11 stand-in, which requires a fresh boot:
    python3 ../rawplay/bench.py --boot --icount 2 --seconds 8 --source test

`--rawplay PATH` benches another build against the one in `out` (the file must be named
`rawplay`, the unit runs `/tmp/rawplay`), and writes `rawplay/out/bench-result-<label>.json`
next to the printed summary.

`--video out.mp4` records the emulated panel (QMP `screendump`) and writes a real-time
video with each captured frame held until the next capture:

    python3 rawplay/bench.py --boot --icount 2 --seconds 10 --source motion --fps 30 \
        --clicks 0 --video rawplay/out/rawplay-run.mp4

Constraints and pitfalls:

* `--icount` requires `--boot`; a non-icount save state must never be resumed under icount.
* `snap.qcow2` is locked by any running qemu: run one emulator at a time, or point
  `SNAPSHOTS` at a copy. QMP accepts a single client; the harness holds it.
* Per-process sockets (`usb.<pid>.sock`, `qmp.<pid>.sock`, ...) make back-to-back runs
  safe; `rawplay/out/` log names are fixed, so serialize benchmark runs.
* Fast visual bench on the snapshot: `cd qemu && python3 tests/snap.py 'upload
  ../out/gfprobe' 'sh /tmp/gfprobe'` (headless, writes `tests/out/sheet.png`).
* to quit the player, press and hold the top-left 96x36 exit region briefly (it is not
  forwarded), or `slay rawplay` from another shell.

### Real car

Put the player on the stick image the gadget carries, then stream the virtual wayland
session to the unit; the unit runs it off `/fs/usb0`, nothing is written to it. The port
must be high speed (the jailbroken `io-usb` restart,
`usb/homebrew/apps/usbhs.sh`): the stock `force_fs` port cannot carry 768 KB/frame.

    make -C rawplay rawlink
    make stick                  # copies the player into usb/ and packs usb.img
    rawplay/livi.sh
    sudo rawplay/out/rawlink run --wayland "$XDG_RUNTIME_DIR/wayland-livi" --stats
    # or the two-process split (the gadget needs root for configfs):
    sudo rawplay/out/rawlink gadget --stick usb.img --sock /tmp/livi-raw.sock
    rawplay/out/rawlink stream /tmp/livi-raw.sock --usb \
        --wayland "$XDG_RUNTIME_DIR/wayland-livi" --stats

    # on the unit
    /fs/usb0/homebrew/apps/rawplay.sh 0 --stats

`rawplay/out/rawlink gadget` is the configfs gadget (functionfs vendor interface + mass
storage, the descriptor bytes the device has always enumerated); `--sock` defaults to
`/tmp/livi-raw.sock`, `--stick` to `usb.img`. The mass storage function still mounts at
`/fs/usb0`.

The unit player is `rawplay --usb [seconds] [--stats] [--direct] [--stage] [--urb BYTES]
[--poll-us US]`. Staged is the default (read into a usbd_alloc ring and memcpy to the
panel); `--direct` opts into the zero-copy path, which is the emulator's path but stalls
the bulk-in pipe on a real unit (section 3). The header is validated against the protocol
(reserved byte, known type, mode length, frame length) because raw pixels have no start
code to resync on. Reads are bounded (15 s before the first byte, 5 s once streaming) and
the exit gesture interrupts a stalled read, so a dead link always ends the run instead of
hanging. A bulk-in error mid-frame is not fatal any more: the frame is dropped and acked
stale and the reader resumes at the next header (section 3). Touches go out as the same
`LI` messages; the top-left 96x36 region is the exit gesture and is not forwarded. A
heartbeat goes out every second and the mode message is refreshed after each one.

### Conventions

* C style: no tabs, 4 spaces, `/* */` comments that explain *why*; `rawplay/rawplay.c` and
  `mediaplayer/mediaplayer.c` are the reference. Keep 32-bit QNX APCS assumptions
  (the ICC2 SDK's `qnx.h`, `usbdi.h` are hand-written prototypes; add names only if the stub
  exports them).
* Do not add a software fallback silently: print it and record it in the docs.
* Performance claims only from `rawplay/bench.py`, with the mode (`--icount 2` vs host speed)
  stated. The emulator is an upper bound, not a measurement.
* The panel is 800x480 RGB565; there is no `/dev/fb` (see section 1.2).

## 6. Follow-ups / future work

* Double-buffered scanout (flip at IPU EOF) to remove tearing without a copy; needs a
  DMA-capable second buffer plus either a physical-address API for `usbd_alloc` memory or
  a driver-provided second panel buffer.
* Understand the real-car `--direct` stall (section 3) so the zero-copy path can be the
  default there too.
* Revisit IPU YUV/CSC if a later QNX graphics driver exposes the foreground layer.
