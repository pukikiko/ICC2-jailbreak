#!/usr/bin/env python3
# benchmark for the raw usb path. one qemu instance, one boot, the same source and guest
# duration every run:
#
#     python3 rawplay/bench.py --boot --icount 2 --seconds 12
#     python3 rawplay/bench.py --boot --icount 2 --source motion --fps 30 --seconds 10
#     python3 rawplay/bench.py --seconds 8                  # host speed, snapshot resume
#     python3 rawplay/bench.py --boot --icount 2 --source motion --fps 30 --seconds 10 \
#         --video rawplay/out/rawplay-run.mp4               # record the emulated panel
#
# pipeline: out/rawplay + rawplay/out/rawlink (raw rgb565, dma straight into the panel's
# scanout buffer where the stack allows it).
#
# the test pattern checks are the same the old mpeg test used (blue box bottom left, red
# box moving along y=80). latency is the host's send timestamp to the guest's displayed
# ack. under --icount the guest clock is instruction time, so fps/ms are the guest's own
# numbers, not wall clock.
import argparse, functools, json, os, re, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
ICC2 = os.environ.get('ICC2_DIR') or os.path.normpath(os.path.join(REPO, '..', '..', '..', 'ICC2'))
sys.path.insert(0, os.path.join(ICC2, 'qemu'))
sys.path.insert(0, os.path.join(ICC2, 'qemu', 'tests'))

OUT = os.path.join(HERE, 'out')
CLICK = (300, 200)
# the usb/homebrew/apps/usbhs.sh sequence: restart the stack so the port comes up
# without force_fs and the already-attached gadget is enumerated deterministically. the
# stock startsys enumeration is occasionally missed on a cold boot and then the stick and
# the vendor interface never appear.
USB_RESTART = ('slay -Q -f -s9 devb-umass 2>/dev/null; slay -Q -f -s9 io-usb_swsa 2>/dev/null; '
               'sleep 1; io-usb_swsa -i20 -r3 -c -d ehci-mx31_swsa ioport=0x43f88100,irq=37,'
               'num_itd=300 & waitfor /dev/io-usb/io-usb 10; devb-umass cam pnp blk cache=2m,'
               'auto=partition,automount=hd0@dos:/fs/usb0,ro dos exe=all; waitfor /fs/usb0 15')
TOTALS_RAW = re.compile(r'rawplay: (\d+) frames, (\d+) drawn, (\d+) stale, (\d+) kb in '
                        r'(\d+) ms \(([\d.]+) fps, ([\d.]+) ms/frame\), (direct|staged)')


def blue_at(path):
    from PIL import Image
    px = Image.open(path).convert('RGB')
    hits = 0
    for x, y in ((30, 400), (50, 410), (70, 420), (40, 430)):
        r, g, b = px.getpixel((x, y))
        if b > 140 and r < 110 and g < 110:
            hits += 1
    return hits


def make_video(result, out_path, fps=30):
    """real-time recording of the emulated panel: each captured frame is held until the
    next capture, so an emulator that cannot keep the cadence shows held frames instead of
    gaps. the capture timestamps are what the assembly uses."""
    from PIL import Image, ImageDraw, ImageFont
    caps = result.get('caps') or []
    if not caps:
        print('bench: no captured panel frames, not writing the video')
        return 1
    t0 = caps[0][0]
    dur = caps[-1][0] - t0
    width, height, bar = 800, 480, 30
    frames = int(dur * fps) + 1
    font_path = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
    font = (ImageFont.truetype(font_path, 20) if os.path.exists(font_path)
            else ImageFont.load_default())
    enc = subprocess.Popen(
        ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y',
         '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{width}x{height + bar}',
         '-r', str(fps), '-i', '-', '-c:v', 'libx264', '-preset', 'veryfast',
         '-pix_fmt', 'yuv420p', '-crf', '18', out_path],
        stdin=subprocess.PIPE)
    cache = [None, None]                         # [index, image]

    def pick(rel):
        lo, hi = 0, len(caps) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if caps[mid][0] - t0 <= rel:
                lo = mid
            else:
                hi = mid - 1
        if cache[0] != lo:
            cache[0], cache[1] = lo, Image.open(caps[lo][1]).convert('RGB')
        return cache[1]

    for i in range(frames):
        rel = i / fps
        canvas = Image.new('RGB', (width, height + bar), 'black')
        canvas.paste(pick(rel), (0, bar))
        d = ImageDraw.Draw(canvas)
        d.text((8, 4), 'rawplay - rgb565, usb', font=font, fill=(96, 240, 120))
        d.text((8, bar + 6), f'{rel:5.2f}s', font=font, fill=(230, 230, 230))
        enc.stdin.write(canvas.tobytes())
    enc.stdin.close()
    rc = enc.wait()
    print(f'bench: wrote {out_path} ({frames} frames, {dur:.1f}s, {fps} fps)')
    return rc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seconds', type=int, default=12, help='guest seconds to run')
    ap.add_argument('--source', choices=('test', 'motion', 'file'), default='test')
    ap.add_argument('--file', help='source file for --source file')
    ap.add_argument('--fps', type=int, default=15, help='host source cadence')
    ap.add_argument('--icount', type=int, metavar='SHIFT',
                    help='lock the guest cpu to 2^SHIFT ns an instruction (needs --boot)')
    ap.add_argument('--boot', action='store_true',
                    help='boot from scratch instead of resuming the snapshot (needed with '
                         '--icount)')
    ap.add_argument('--stage', action='store_true',
                    help='run rawplay with --stage (announced one-copy fallback)')
    ap.add_argument('--direct', action='store_true',
                    help='run rawplay with --direct (zero-copy; the emulator path the '
                         'representative table in rawplay/README.md was measured on)')
    ap.add_argument('--window', type=int, default=2,
                    help='unacked frames the host keeps in flight (default 2)')
    ap.add_argument('--urb', type=int, default=None,
                    help='bulk-in urb size passed to rawplay (default: its own 262144)')
    ap.add_argument('--poll-us', type=int, default=None,
                    help='rawplay input poll interval in microseconds (default: its own)')
    ap.add_argument('--clicks', type=int, default=3, help='touch round-trip samples')
    ap.add_argument('--climate', action='store_true',
                    help='press a climate panel key with the video running and check the hmi '
                         'window: the bar comes back for a moment, the video pauses and '
                         'resumes, and a non-climate key does neither (resume only)')
    ap.add_argument('--vehicle', action='store_true',
                    help='drive day/night and gear on the v850 with the video running: a '
                         'day/night switch opens the 1 s hmi window, reverse holds it open '
                         'until the gear leaves reverse, and the video resumes after each')
    ap.add_argument('--snapshots', metavar='QCOW2')
    ap.add_argument('--video', metavar='MP4',
                    help='capture the emulated panel during the run and write a real-time '
                         'mp4 (the capture costs the emulator some time, so the numbers '
                         'printed by this run are not the reference ones)')
    ap.add_argument('--video-fps', type=int, default=30, help='mp4 playback rate')
    ap.add_argument('--rawplay', metavar='PATH',
                    default=os.path.join(REPO, 'out', 'rawplay'),
                    help='unit binary to upload as /tmp/rawplay, for benching another build')
    ap.add_argument('--label', default='new',
                    help='names the result json (bench-result-<label>.json)')
    args = ap.parse_args()

    if args.icount is not None and not args.boot:
        print('bench: --icount needs --boot (a save state cannot be resumed with icount)')
        return 2
    if args.snapshots:
        os.environ['SNAPSHOTS'] = args.snapshots
    os.makedirs(OUT, exist_ok=True)

    import upload, v850                                        # noqa: E402
    from guest import (GDB_SOCKET, HERE as QEMU_HERE, SER5_SOCKET, UART3_SOCKET,  # noqa: E402
                       USB_SOCKET, boot, mount_root, resume, run, stage, type_line)
    from hmi import USB_START                                  # noqa: E402
    from snap import Qmp                                       # noqa: E402

    stick = os.path.join(REPO, 'usb.img')
    if not os.path.exists(stick):
        print(f'bench: {stick} is missing, run mkusb.py first')
        return 2
    qmp_path = f'{OUT}/bench-qmp.{os.getpid()}.sock'
    extra = ['-display', 'none', '-qmp', f'unix:{qmp_path},server=on,wait=off',
             '-chardev', f'socket,id=liviusb,path={USB_SOCKET},server=on,wait=off',
             '-drive', f'if=none,id=stick,format=raw,read-only=on,file={stick}']
    if args.boot:
        # a cold boot enumerates the gadget itself, the way hmi.py --livi-usb does; a
        # resumed state is hotplugged after loadvm instead. chunked is the raw transfer
        # mode: request-sized bulk transfers, which the direct ring dma's whole chunks with.
        extra += ['-device', 'usb-livi,drive=stick,chardev=liviusb,id=liviusb0,chunked=on']
    if args.icount is not None:
        extra += ['-icount', f'shift={args.icount},sleep=off']
    console = open(f'{OUT}/bench-console.log', 'w')
    if args.boot:
        etfs = QEMU_HERE + '/../dump/packages/factory/qnx_binaries_and_libraries/root/sbin/fs-etfs-ram'
        root = QEMU_HERE + '/stage.tar.gz'
        g = boot(extra + stage(etfs, 0) + stage(root, 1 << 20), log=console)
        mount_root(g, etfs, root)
        run(g, 'export boardVersion=3.50 boardVariant=high')
        run(g, '. /proc/boot/pkgstart.sh >/dev/null 2>&1', timeout=600)
        time.sleep(10)
        micro = None
    else:
        g = resume('started', extra, log=console)
        open(f'{OUT}/bench-v850.log', 'w').close()
        micro = v850.V850(v850.connect(UART3_SOCKET),
                          lambda m: print(m, file=open(f'{OUT}/bench-v850.log', 'a'),
                                          flush=True))
        micro.uploader = functools.partial(upload.upload, GDB_SOCKET)
        time.sleep(2)
    q = Qmp(qmp_path)

    def guest_upload(files):
        if micro:
            v850.command(micro, 'upload ' + ' '.join(files))
            deadline = time.time() + 180
            while time.time() < deadline:
                text = guest_text()
                if 'dd if=/dev/mem' in text and '#' in text.split('dd if=/dev/mem')[-1]:
                    return True
                time.sleep(0.3)
            return False
        run(g, upload.upload(GDB_SOCKET, files), timeout=300)
        return True

    def guest_start(line):
        if micro:
            v850.command(micro, line)
        else:
            type_line(g, line)

    def guest_text():
        if micro:
            text = open(f'{OUT}/bench-v850.log').read()
            text = re.sub(r'rx ch 0x[0-9a-f]+:.*\n', '', text)
            text = text.replace('cansole: ', '')
        else:
            text = getattr(g, 'before', '') or ''
        return text.replace('\x07', '').replace('\n', '')

    def wait_text(pattern, timeout, after=None):
        deadline = time.time() + timeout
        while time.time() < deadline:
            text = guest_text()
            if after is not None:
                text = text[text.find(after) + 1:] if after in text else ''
            if re.search(pattern, text):
                return True
            time.sleep(0.4)
        return False

    def guest_expect(pattern, timeout):
        """boot mode only: wait for the pexpect console to show pattern, keep g.before"""
        if micro:
            return wait_text(pattern, timeout)
        try:
            g.expect(pattern, timeout=timeout)
            return True
        except Exception:                                       # noqa: BLE001
            return False

    HMI_OPEN = 'rawplay: climate key, hmi back'
    HMI_CLOSE = 'rawplay: climate window closed'
    BTN_OPEN = 'rawplay: buttons restarted for the hmi'
    BTN_CLOSE = 'rawplay: buttons stopped with the hmi again'

    def climate_check(kind, events):
        """press a climate key while the video runs: the player pauses the stream, brings the
        hmi back so the climate bar shows the press, then resumes. a pause shows up as a gap
        in the host's displayed-frame acks; a non-climate key must not open a window.
        a second climate key follows the first: the hmi stop/resume cycle of the first window
        must not leave the panel deaf to the next press. buttons is paused for the session, so
        each window must restart it clean (the queued session backlog dies with the old
        process) and stop it again."""
        from PIL import Image, ImageChops
        if not micro:
            print('bench: --climate needs the v850 (use the resume path, not --boot)')
            return {'ok': False, 'windows': -1, 'pause': 0.0}
        text0 = guest_text()
        before_open, before_close = text0.count(HMI_OPEN), text0.count(HMI_CLOSE)
        before_btn, before_btn_close = text0.count(BTN_OPEN), text0.count(BTN_CLOSE)
        q.shot(f'{OUT}/bench-{kind}-video2.ppm')
        v850.command(micro, 'key ac')
        time.sleep(0.6)
        q.shot(f'{OUT}/bench-{kind}-during.ppm')
        time.sleep(2.5)
        q.shot(f'{OUT}/bench-{kind}-after.ppm')
        v850.command(micro, 'key auto')             # the second press: the regression
        time.sleep(0.6)
        q.shot(f'{OUT}/bench-{kind}-during2.ppm')
        time.sleep(2.5)
        q.shot(f'{OUT}/bench-{kind}-after2.ppm')
        v850.command(micro, 'tap 23')               # cd/aux, not a climate key
        time.sleep(0.6)
        clicks = []
        for _ in range(2):                          # touch must still reach the host after
            time.sleep(1.2)
            clicks.append(time.monotonic())
            q.click(*CLICK)
        time.sleep(1.0)
        text = guest_text()
        windows = text.count(HMI_OPEN) - before_open
        closed = text.count(HMI_CLOSE) - before_close
        btn_windows = text.count(BTN_OPEN) - before_btn
        btn_closed = text.count(BTN_CLOSE) - before_btn_close
        touch_after = False
        for line in open(events):
            p = line.split()
            if len(p) >= 5 and p[1] == 'touch':
                try:
                    if float(p[0]) >= clicks[0] and int(p[4]):
                        touch_after = True
                except ValueError:
                    pass
        drawn = []
        for line in open(events):
            p = line.split()
            if len(p) >= 3 and p[1] == 'drawn':
                try:
                    drawn.append(float(p[0]))
                except ValueError:
                    pass
        gaps = [b - a for a, b in zip(drawn, drawn[1:])]
        pauses = sorted(gaps, reverse=True)[:2]
        pause = max(gaps) if gaps else 0.0

        def diff(a, b):
            ia = Image.open(a).convert('RGB')
            ib = Image.open(b).convert('RGB')
            return ImageChops.difference(ia, ib).getbbox() is not None

        bar_back = diff(f'{OUT}/bench-{kind}-video2.ppm', f'{OUT}/bench-{kind}-during.ppm')
        resumed = diff(f'{OUT}/bench-{kind}-during.ppm', f'{OUT}/bench-{kind}-after.ppm')
        bar_back2 = diff(f'{OUT}/bench-{kind}-after.ppm', f'{OUT}/bench-{kind}-during2.ppm')
        resumed2 = diff(f'{OUT}/bench-{kind}-during2.ppm', f'{OUT}/bench-{kind}-after2.ppm')
        ok = (windows == 2 and closed == 2 and btn_windows == 2 and btn_closed == 2 and
              0.7 <= pause <= 4.0 and
              bar_back and resumed and bar_back2 and resumed2 and touch_after)
        if args.source == 'test':
            ok = ok and blue_at(f'{OUT}/bench-{kind}-after.ppm') >= 3
            ok = ok and blue_at(f'{OUT}/bench-{kind}-after2.ppm') >= 3
        print(f'bench: climate: {windows} window(s), {closed} close(s), {btn_windows} buttons '
              f'window(s), {btn_closed} buttons close(s), drawn pauses '
              f'{pauses}, hmi back {bar_back}/{bar_back2}, video resumed {resumed}/{resumed2}, '
              f'touch after {touch_after}')
        return {'ok': ok, 'windows': windows, 'buttons': btn_windows, 'pause': pause}

    NIGHT_OPEN = 'rawplay: day/night switch, hmi back'
    NIGHT_CLOSE = 'rawplay: day/night window closed'
    REV_OPEN = 'rawplay: reverse gear, hmi back'
    REV_CLOSE = 'rawplay: reverse window closed'

    def vehicle_check(kind, events):
        """drive the car while the video runs: a day/night switch opens the same 1 s window
        a climate key opens and the video comes back on its own; reverse brings the hmi
        back and the window stays past that deadline for as long as the gear is engaged,
        closing (and resuming the video) when it leaves reverse. the second day/night
        switch proves the reader recovers after the first window's hmi stop/resume, the
        same regression the climate check covers."""
        from PIL import Image, ImageChops
        if not micro:
            print('bench: --vehicle needs the v850 (use the resume path, not --boot)')
            return {'ok': False}
        text0 = guest_text()
        n_open, n_close = text0.count(NIGHT_OPEN), text0.count(NIGHT_CLOSE)
        r_open, r_close = text0.count(REV_OPEN), text0.count(REV_CLOSE)
        q.shot(f'{OUT}/bench-{kind}-veh-video.ppm')
        v850.command(micro, 'night 1')
        time.sleep(0.6)
        q.shot(f'{OUT}/bench-{kind}-veh-night.ppm')
        time.sleep(1.6)
        q.shot(f'{OUT}/bench-{kind}-veh-after-night.ppm')
        v850.command(micro, 'night 0')
        time.sleep(2.2)                     # the second window open and close
        q.shot(f'{OUT}/bench-{kind}-veh-after-night2.ppm')
        v850.command(micro, 'gear reverse')
        time.sleep(0.8)
        q.shot(f'{OUT}/bench-{kind}-veh-reverse.ppm')
        time.sleep(2.5)                     # past the 1 s deadline: must still be up
        held = guest_text().count(REV_CLOSE) - r_close
        v850.command(micro, 'gear forward')
        time.sleep(1.5)
        q.shot(f'{OUT}/bench-{kind}-veh-after-reverse.ppm')
        text = guest_text()
        windows = text.count(NIGHT_OPEN) - n_open
        closed = text.count(NIGHT_CLOSE) - n_close
        rev_windows = text.count(REV_OPEN) - r_open
        rev_closed = text.count(REV_CLOSE) - r_close

        def diff(a, b):
            ia = Image.open(a).convert('RGB')
            ib = Image.open(b).convert('RGB')
            return ImageChops.difference(ia, ib).getbbox() is not None

        bar_back = diff(f'{OUT}/bench-{kind}-veh-video.ppm', f'{OUT}/bench-{kind}-veh-night.ppm')
        resumed = diff(f'{OUT}/bench-{kind}-veh-night.ppm',
                       f'{OUT}/bench-{kind}-veh-after-night.ppm')
        # the test pattern's red box does not move (the lavfi t is not per frame), so the
        # second resume is checked against the first window's static hmi shot, and the blue
        # box marker is the video's proof on the panel shots (the hmi does not have it)
        resumed2 = diff(f'{OUT}/bench-{kind}-veh-night.ppm',
                        f'{OUT}/bench-{kind}-veh-after-night2.ppm')
        rev_back = diff(f'{OUT}/bench-{kind}-veh-after-night2.ppm',
                        f'{OUT}/bench-{kind}-veh-reverse.ppm')
        rev_resumed = diff(f'{OUT}/bench-{kind}-veh-reverse.ppm',
                           f'{OUT}/bench-{kind}-veh-after-reverse.ppm')
        ok = (windows == 2 and closed == 2 and rev_windows == 1 and rev_closed == 1 and
              held == 0 and bar_back and resumed and resumed2 and rev_back and rev_resumed)
        if args.source == 'test':
            ok = ok and blue_at(f'{OUT}/bench-{kind}-veh-night.ppm') == 0
            ok = ok and blue_at(f'{OUT}/bench-{kind}-veh-reverse.ppm') == 0
            ok = ok and blue_at(f'{OUT}/bench-{kind}-veh-after-night.ppm') >= 3
            ok = ok and blue_at(f'{OUT}/bench-{kind}-veh-after-night2.ppm') >= 3
            ok = ok and blue_at(f'{OUT}/bench-{kind}-veh-after-reverse.ppm') >= 3
        print(f'bench: vehicle: {windows} day/night window(s), {closed} close(s), '
              f'{rev_windows} reverse window(s), {rev_closed} close(s), closes while held '
              f'{held}, hmi back {bar_back}/{rev_back}, video resumed {resumed}/{resumed2}/'
              f'{rev_resumed}')
        return {'ok': ok, 'windows': windows, 'closed': closed, 'rev_windows': rev_windows}

    # the composite gadget delivers request-sized transfers (chunked) so the raw path can
    # dma whole chunks into the panel; the same device also carries the homebrew stick.
    if not args.boot:
        reply = q.cmd('device_add', driver='usb-livi', id='liviusb0', drive='stick',
                      chardev='liviusb', chunked=True)
        if 'error' in reply:
            print(f'bench: could not plug the gadget: {reply["error"]}')
            return 2
        time.sleep(4)
        print('bench: starting the usb stack')
        if micro:
            v850.command(micro, 'sh ' + USB_START)
            time.sleep(6)
    stick_ok = False
    for attempt in range(6):
        if micro:
            v850.command(micro, 'sh ls /fs/usb0/homebrew')
            stick_ok = wait_text(r'apps\.txt|buttons\.txt', 25)
        else:
            stick_ok = 'apps.txt' in run(g, 'ls /fs/usb0/homebrew', timeout=60)
        if stick_ok:
            break
        if attempt == 1:
            print('bench: restarting the usb stack (usbhs.sh sequence)')
            if micro:
                v850.command(micro, 'sh ' + USB_RESTART)
                time.sleep(8)
            else:
                run(g, USB_RESTART, timeout=180)
    print(f'bench: homebrew stick at /fs/usb0: {stick_ok}')

    if os.path.basename(args.rawplay) != 'rawplay':
        print('bench: --rawplay must be named rawplay, the unit side runs /tmp/rawplay')
        g.terminate(force=True)
        return 2
    files = [args.rawplay, os.path.join(REPO, 'rawplay', 'rawplay.sh')]
    if not guest_upload(files):
        print('bench: the upload never finished')
        g.terminate(force=True)
        return 2
    time.sleep(1)

    deadline_scale = args.seconds * (5 if args.icount else 1) + 90
    results = {}

    def run_pipeline(kind):
        print(f'bench: raw pipeline, {args.seconds} guest seconds')
        events = f'{OUT}/bench-events-{kind}.log'
        streamlog = f'{OUT}/bench-stream-{kind}.log'
        for stale in (events, streamlog):
            if os.path.exists(stale):
                os.unlink(stale)
        player = (f'sh /tmp/rawplay.sh {args.seconds} --stats'
                  + (f' --urb {args.urb}' if args.urb else '')
                  + (f' --poll-us {args.poll_us}' if args.poll_us else '')
                  + (' --direct' if args.direct else ' --stage' if args.stage else ''))
        sender = os.path.join(HERE, 'out', 'rawlink')
        if not os.path.exists(sender):
            print(f'bench: {sender} not built (make -C rawplay rawlink)')
            return None
        stream_cmd = [sender, 'stream', USB_SOCKET, '--usb', '--events', events, '--stats',
                      '--seconds', str(deadline_scale), '--window', str(args.window)]
        if args.source == 'motion':
            stream_cmd += ['--motion', '--fps', str(args.fps)]
        elif args.source == 'file':
            stream_cmd += ['--file', args.file, '--fps', str(args.fps)]
        else:
            stream_cmd += ['--test', '--fps', str(args.fps)]

        marker = 'rawplay: usb gadget up'
        guest_start(player)
        if micro:
            if not wait_text(marker, deadline_scale):
                print(f'bench: {kind}: the player never started')
                return None
        else:
            if not guest_expect(marker, 120):
                print(f'bench: {kind}: the player never started')
                return None
        wall0 = time.time()
        # panel capture for --video: the qmp client serialises screendumps and clicks, so
        # a background thread can record while the run goes on. the timestamps are what
        # the assembly uses, an emulator that cannot keep the cadence just holds frames.
        caps = []
        cap_stop = threading.Event()
        cap_thread = None
        if args.video:
            vdir = f'{OUT}/video-{kind}'
            os.makedirs(vdir, exist_ok=True)
            for f in os.listdir(vdir):
                os.unlink(os.path.join(vdir, f))

            def capture():
                i = 0
                while not cap_stop.is_set():
                    path = f'{vdir}/{i:05d}.ppm'
                    t0 = time.monotonic()
                    try:
                        q.cmd('screendump', filename=path)
                        if os.path.exists(path):
                            caps.append(((t0 + time.monotonic()) / 2, path))
                    except Exception as e:                      # noqa: BLE001
                        print(f'bench: capture: {e}')
                    i += 1
                    dt = time.monotonic() - t0
                    if dt < 0.05:
                        time.sleep(0.05 - dt)
            cap_thread = threading.Thread(target=capture, daemon=True)
            cap_thread.start()
        stream = subprocess.Popen(stream_cmd, stdout=open(streamlog, 'w'),
                                  stderr=subprocess.STDOUT, start_new_session=True)
        clicks = []
        time.sleep(3)
        shot = f'{OUT}/bench-{kind}.ppm'
        if args.source == 'test':
            q.shot(shot)
        for i in range(args.clicks):
            time.sleep(1.2)
            clicks.append(time.monotonic())     # the event logs are monotonic too
            q.click(*CLICK)
        climate = climate_check(kind, events) if args.climate else None
        vehicle = vehicle_check(kind, events) if args.vehicle else None
        if micro:
            wait_text(r'rawplay: \d+ frames, \d+ drawn, \d+ stale, \d+ kb in', deadline_scale)
            # the console channel delivers the player's final line in chunks, and the first
            # one already matches the wait above: wait for the whole totals line so the
            # parse below never runs before its tail has arrived
            wait_text(TOTALS_RAW.pattern, 10)
        else:
            guest_expect(r'@@DONE', deadline_scale)
        cap_stop.set()
        if cap_thread:
            cap_thread.join(timeout=5)
        wall = time.time() - wall0
        try:
            os.killpg(stream.pid, 15)
        except ProcessLookupError:
            pass
        try:
            stream.wait(timeout=10)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(stream.pid, 9)
            except ProcessLookupError:
                pass
            stream.wait()
        text = guest_text()
        res = {'wall': wall, 'kind': kind, 'events': events, 'streamlog': streamlog,
               'shot': shot if os.path.exists(shot) else None, 'clicks': clicks,
               'caps': caps, 'climate': climate, 'vehicle': vehicle}
        m = TOTALS_RAW.search(text)
        if not m:
            print(f'bench: {kind}: no totals line found')
            return None
        res.update(frames=int(m.group(1)), drawn=int(m.group(2)), stale=int(m.group(3)),
                   fps=float(m.group(6)), ms_frame=float(m.group(7)),
                   mode=m.group(8), direct=(m.group(8) == 'direct'))
        res['wall_fps'] = res['frames'] / wall if wall else 0.0
        # latency comes from the host's own event log: `drawn <seq> <ms>` written when the
        # guest's displayed ack came back. the first few samples can be stale (frames
        # already in flight when the player started), so the average is over the samples
        # after the first three.
        lats = []
        if os.path.exists(events):
            for line in open(events):
                p = line.split()
                if len(p) >= 4 and p[1] == 'drawn':
                    try:
                        lats.append(float(p[3]))
                    except ValueError:
                        pass
        lats = lats[3:] if len(lats) > 6 else lats
        if lats:
            lats.sort()
            res['lat_avg'] = sum(lats) / len(lats)
            res['lat_med'] = lats[len(lats) // 2]
            res['lat_p95'] = lats[min(len(lats) - 1, int(len(lats) * 0.95))]
            res['lat_max'] = lats[-1]
            res['lat_n'] = len(lats)
        else:
            res['lat_avg'] = res['lat_med'] = res['lat_p95'] = res['lat_max'] = 0.0
            res['lat_n'] = 0
        if args.source == 'test' and res['shot']:
            res['blue'] = blue_at(res['shot'])
        # touch round trip: the first touch event at or after each click
        rt = []
        if os.path.exists(events):
            ev = []
            for line in open(events):
                parts = line.split()
                if len(parts) >= 5 and parts[1] == 'touch':
                    try:
                        ev.append((float(parts[0]), int(parts[4])))
                    except ValueError:
                        pass
            for c in clicks:
                cands = [(t - c) * 1000.0 for t, down in ev if t >= c and down]
                if cands:
                    rt.append(min(cands))
        res['touch_ms'] = sum(rt) / len(rt) if rt else None
        print(f'bench: {kind}: {res["frames"]} frames, {res["fps"]:.1f} guest fps, '
              f'{res["ms_frame"]:.1f} ms/frame, latency {res["lat_avg"]:.1f}/{res["lat_max"]:.1f} ms'
              + (f', touch {res["touch_ms"]:.1f} ms' if res['touch_ms'] is not None else '')
              + (f', {res["mode"]}' if res.get('mode') else ''))
        return res

    r = run_pipeline('new')
    if r is None:
        g.terminate(force=True)
        return 1
    results['new'] = r
    with open(f'{OUT}/bench-result-{args.label}.json', 'w') as f:
        json.dump({k: v for k, v in r.items() if k != 'caps'}, f, indent=1)

    print('\n=== bench summary (%s, %s) ===' % (args.label,
          '--icount ' + str(args.icount) if args.icount else 'host speed'))
    res = results['new']
    hdr = f'{"metric":32} {"rawplay+raw":>16}'
    print(hdr)
    print('-' * len(hdr))

    def row(name, key, fmt='{:.1f}'):
        v = res.get(key)
        print(f'{name:32} {("n/a" if v is None else fmt.format(v)):>16}')

    row('frames', 'frames', '{}')
    row('fps (guest clock)', 'fps')
    row('fps (wall clock)', 'wall_fps')
    row('ms/frame (guest)', 'ms_frame')
    row('latency avg ms', 'lat_avg')
    row('latency median ms', 'lat_med')
    row('latency p95 ms', 'lat_p95')
    row('latency max ms', 'lat_max')
    row('latency samples', 'lat_n', '{}')
    row('touch round trip ms', 'touch_ms')
    row('test pattern blue hits', 'blue', '{}')
    print(f'{"usb path":32} {res.get("mode", "n/a"):>16}')
    print(f'{"homebrew stick mounted":32} {"yes" if stick_ok else "no":>16}')
    ok = True
    checks = [
        ('stick mounted', stick_ok),
        ('touch reached host', res['touch_ms'] is not None),
        ('frames displayed', res['frames'] > 0),
    ]
    if args.source == 'test':
        checks.append(('pattern on panel', res.get('blue', 0) >= 3))
    if args.climate:
        climate = res.get('climate') or {}
        print(f'{"climate windows":32} {climate.get("windows", "n/a"):>16}')
        print(f'{"climate buttons windows":32} {climate.get("buttons", "n/a"):>16}')
        print(f'{"climate pause s":32} {climate.get("pause", 0.0):>16.1f}')
        checks.append(('climate hmi window', bool(climate.get('ok'))))
    if args.vehicle:
        vehicle = res.get('vehicle') or {}
        print(f'{"day/night windows":32} {vehicle.get("windows", "n/a"):>16}')
        print(f'{"day/night closes":32} {vehicle.get("closed", "n/a"):>16}')
        print(f'{"reverse windows":32} {vehicle.get("rev_windows", "n/a"):>16}')
        checks.append(('vehicle hmi window', bool(vehicle.get('ok'))))
    print()
    for name, passed in checks:
        print(('PASS' if passed else 'FAIL') + f': {name}')
        ok = ok and passed
    if args.video:
        if make_video(res, args.video, args.video_fps) != 0:
            ok = False
    g.terminate(force=True)
    for p in (qmp_path, UART3_SOCKET, SER5_SOCKET, GDB_SOCKET, USB_SOCKET):
        if os.path.exists(p):
            os.unlink(p)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
