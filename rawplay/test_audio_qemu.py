#!/usr/bin/env python3
# end-to-end audio test in the ICC2 emulator: the unit's rawplay opens the real QNX PCM
# devices, the host side is rawlink stream over the usb-livi chardev, and qemu's wav
# backend captures the codec output. run after `make out/rawplay` and `make -C rawplay rawlink`.
import os, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
ICC2 = os.environ.get('ICC2_DIR', os.path.join(HERE, '..', '..', 'ICC2'))
sys.path.insert(0, os.path.join(ICC2, 'qemu'))

import hmi, qmp, v850, upload
from guest import (GDB_SOCKET, HERE as QEMU_HERE, QMP_SOCKET, SER5_SOCKET, UART3_SOCKET,
                   USB_SOCKET, resume)

RAWPLAY = os.path.join(HERE, '..', 'out', 'rawplay')
RAWLINK = os.path.join(HERE, 'out', 'rawlink')
WAV = '/tmp/icc-audio-qemu.wav'
MIC = '/tmp/icc-mic-qemu.raw'
CONSOLE = '/tmp/icc-audio-console.log'
HOSTLOG = '/tmp/icc-audio-host.log'
PA = os.environ.get('AUDIO_QEMU_PA') == '1'
PA_OUT = '/tmp/icc-audio-pa.raw'
PA_FIFO = '/tmp/icc-test-mic.fifo'
pa_modules = []
pa_stop = threading.Event()
pa_feed = None
pa_rec = None

g = None
micro = None
host = None


def log(msg):
    print(msg, flush=True)


def pa_setup():
    """a null sink captures qemu's output and a pipe source feeds it a known mic tone."""
    global pa_feed, pa_rec
    try:
        os.unlink(PA_FIFO)
    except FileNotFoundError:
        pass
    mod = subprocess.check_output(
        ['pactl', 'load-module', 'module-null-sink', 'sink_name=icc-test-null'],
        text=True).strip()
    pa_modules.append(mod)
    mod = subprocess.check_output(
        ['pactl', 'load-module', 'module-pipe-source', 'source_name=icc-test-mic',
         f'file={PA_FIFO}', 'format=s16le', 'rate=44100', 'channels=2'],
        text=True).strip()
    pa_modules.append(mod)
    os.environ['PULSE_SINK'] = 'icc-test-null'
    os.environ['PULSE_SOURCE'] = 'icc-test-mic'
    pa_rec = subprocess.Popen(
        ['parec', '--device=icc-test-null.monitor', '--format=s16le', '--rate=44100',
         '--channels=2', '--raw'], stdout=open(PA_OUT, 'wb'), stderr=subprocess.DEVNULL)

    def feed():
        import math, struct
        phase = 0.0
        step = 2.0 * math.pi * 440.0 / 44100.0
        with open(PA_FIFO, 'wb', buffering=0) as f:
            while not pa_stop.is_set():
                buf = bytearray()
                for _ in range(441):
                    v = int(math.sin(phase) * 6000)
                    buf += struct.pack('<hh', v, v)
                    phase += step
                    if phase > 2.0 * math.pi:
                        phase -= 2.0 * math.pi
                try:
                    f.write(buf)
                except (BrokenPipeError, OSError):
                    return
                time.sleep(0.01)

    pa_feed = threading.Thread(target=feed, daemon=True)
    pa_feed.start()
    time.sleep(0.5)


def pa_teardown():
    global pa_rec
    pa_stop.set()
    if pa_rec:
        pa_rec.terminate()
        try:
            pa_rec.wait(timeout=3)
        except subprocess.TimeoutExpired:
            pa_rec.kill()
        pa_rec = None
    for m in pa_modules:
        subprocess.run(['pactl', 'unload-module', m], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    pa_modules.clear()
    for k in ('PULSE_SINK', 'PULSE_SOURCE'):
        os.environ.pop(k, None)


def main():
    global g, micro, host
    if not os.path.exists(RAWPLAY):
        raise SystemExit(f'missing {RAWPLAY}; run make out/rawplay')
    if not os.path.exists(RAWLINK):
        raise SystemExit(f'missing {RAWLINK}; run make -C rawplay rawlink')
    for p in (WAV, MIC, CONSOLE, HOSTLOG, USB_SOCKET, QMP_SOCKET, UART3_SOCKET,
              SER5_SOCKET, GDB_SOCKET):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    extra = ['-display', 'none', '-qmp', f'unix:{QMP_SOCKET},server=on,wait=off',
             '-drive', f'if=none,id=stick,format=raw,read-only=on,file={os.path.join(HERE, "..", "usb.img")}',
             '-chardev', f'socket,id=liviusb,path={USB_SOCKET},server=on,wait=off']
    if PA:
        pa_setup()
        extra += ['-audiodev', 'pa,id=snd', '-global', 'wm8580.audiodev=snd']
    else:
        extra += ['-audiodev', f'wav,id=snd,path={WAV}',
                  '-global', 'wm8580.audiodev=snd']
    log('resuming the started snapshot')
    g = resume('started', extra, log=open(CONSOLE, 'w'))
    micro = v850.V850(v850.connect(UART3_SOCKET), lambda m: None)
    micro.uploader = lambda paths: upload.upload(GDB_SOCKET, paths)
    q = qmp.Qmp(QMP_SOCKET)
    time.sleep(2)

    log('plugging the livi gadget')
    hmi.plug_livi_usb(q)
    hmi.run(g, hmi.USB_START, timeout=60)

    log('uploading rawplay')
    v850.command(micro, f'upload {RAWPLAY}')
    time.sleep(8)
    # make sure the tar finished before the shell tries to exec the file
    hmi.run(g, 'ls -l /tmp/rawplay; cksum /tmp/rawplay; cksum /fs/usb0/homebrew/apps/rawplay',
            timeout=20)
    time.sleep(1)

    unit = threading.Thread(
        target=lambda: hmi.run(g, '/tmp/rawplay --usb --stats 8', timeout=20),
        daemon=True)
    unit.start()
    time.sleep(1)

    log('starting host rawlink with a test tone')
    with open(HOSTLOG, 'w') as hlog:
        host = subprocess.Popen([RAWLINK, 'stream', USB_SOCKET, '--test',
                                 '--audio-test', '--audio-out', MIC,
                                 '--seconds', '9', '--stats'],
                                stdout=hlog, stderr=subprocess.STDOUT)
        host.wait(timeout=20)
    unit.join(timeout=12)
    time.sleep(1)

    print(open(HOSTLOG).read())
    return check()


def check():
    ok = True
    source = micro.heard.get('acm msg 0x10') if micro else None
    log(f"acm source request: {source['payload'] if source else '(none)'}")
    if not source or not source['payload'].startswith('10 03 05 00 00 00 0b'):
        log('FAIL rawplay did not select ACM media mode 11 (USB audio)')
        ok = False
    src = PA_OUT if PA else WAV
    if not os.path.exists(src) or os.path.getsize(src) < 1000:
        log(f'FAIL no qemu audio ({src})')
        ok = False
    else:
        data = open(src, 'rb').read()
        pcm = data[44:] if (not PA and data[:4] == b'RIFF') else data
        if not pcm:
            log('FAIL empty qemu audio')
            ok = False
        else:
            peak = max(abs(int.from_bytes(pcm[i:i + 2], 'little', signed=True))
                       for i in range(0, len(pcm) - 1, 2))
            log(f'qemu audio: {len(pcm)} PCM bytes, peak {peak}')
            if peak < 200:
                log('FAIL qemu audio is silent')
                ok = False
    if not os.path.exists(MIC) or os.path.getsize(MIC) < 1000:
        log(f'FAIL no microphone capture ({MIC})')
        ok = False
    else:
        d = open(MIC, 'rb').read()
        zc = sum(1 for a, b in zip(d, d[1:]) if (a < 128) != (b < 128))
        log(f'microphone: {len(d)} bytes, {len(d) // 160} chunks, '
            f'min {min(d)} max {max(d)}, zero crossings {zc}')
        if PA and zc < 100:
            log('FAIL microphone capture has no tone')
            ok = False
    log('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    finally:
        try:
            if host and host.poll() is None:
                host.terminate()
        except Exception:
            pass
        if g:
            g.terminate(force=True)
        pa_teardown()
        for p in (USB_SOCKET, QMP_SOCKET, UART3_SOCKET, SER5_SOCKET, GDB_SOCKET):
            try:
                os.unlink(p)
            except FileNotFoundError:
                pass
