#!/usr/bin/env python3
# run the ICC emulator as the car end of the physical appliance: the appliance's USB
# gadget is claimed on this host and its vendor bulk bytes are relayed to qemu's
# usb-livi chardev. the guest runs the stick's own rawplay.sh, so the player is the one
# the stick carries, and it sees the same LR/LI stream the real car sees.
#
#   python3 rawplay/run_emu_appliance.py            # gtk window + host audio via pacat
#   EMU_AUDIO=pa python3 rawplay/run_emu_appliance.py
#   EMU_AUDIO=wav python3 rawplay/run_emu_appliance.py
#   EMU_DISPLAY=none python3 rawplay/run_emu_appliance.py
#
# stop with Ctrl-C or `kill $(cat /tmp/emu-appliance.pid)`.
import os, signal, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
JB = os.path.dirname(HERE)
ICC = os.environ.get('ICC2_DIR', os.path.join(JB, '..', 'ICC2'))
sys.path.insert(0, os.path.join(ICC, 'qemu'))

import hmi, qmp
import pexpect
from guest import (GDB_SOCKET, QMP_SOCKET, SER5_SOCKET, UART3_SOCKET, USB_SOCKET,
                   resume, type_line)
STICK = os.path.join(JB, 'usb.img')
BRIDGE = os.path.join(HERE, 'out', 'rawplay-linux')
# the unit runs the player the way the stick's own launcher does, not a /tmp upload
STICK_RAWPLAY = '/fs/usb0/homebrew/apps/rawplay.sh'
CONSOLE = '/tmp/emu-appliance-console.log'
BRIDGE_LOG = '/tmp/emu-appliance-bridge.log'
WAV = '/tmp/emu-appliance.wav'
AUDIO_FIFO = '/tmp/emu-appliance-audio.fifo'
PIDFILE = '/tmp/emu-appliance.pid'
VNC_MODE = os.environ.get('EMU_VNC', '')
DISPLAY_MODE = os.environ.get('EMU_DISPLAY') or ('vnc' if VNC_MODE else 'gtk')
VNC = VNC_MODE or '127.0.0.1:1'
AUDIO = os.environ.get('EMU_AUDIO', 'pacat')

stop = threading.Event()
g = None
bridge = None
audio_player = None


def status(msg):
    print(msg, flush=True)


def drain():
    while not stop.is_set():
        try:
            g.read_nonblocking(4096, timeout=1)
        except pexpect.TIMEOUT:
            continue
        except pexpect.EOF:
            return
        except Exception:
            return


def on_signal(signum, frame):
    stop.set()


def audio_fifo_player():
    """qemu's wav backend is the one backend that reliably keeps the emulated codec
    running; feed its raw s16le/44100/stereo output straight into pacat for host audio."""
    global audio_player
    try:
        os.unlink(AUDIO_FIFO)
    except FileNotFoundError:
        pass
    os.mkfifo(AUDIO_FIFO, 0o666)
    fd = os.open(AUDIO_FIFO, os.O_RDWR)
    audio_player = subprocess.Popen(
        ['pacat', '--playback', '--format=s16le', '--rate=44100', '--channels=2',
         '--raw', '--latency-msec=50'],
        stdin=fd, stdout=subprocess.DEVNULL,
        stderr=open('/tmp/emu-appliance-audio.log', 'w'))
    os.close(fd)


def audio_fifo_cleanup():
    global audio_player
    if audio_player is not None:
        try:
            audio_player.terminate()
            audio_player.wait(timeout=3)
        except Exception:
            try:
                audio_player.kill()
            except Exception:
                pass
        audio_player = None
    try:
        os.unlink(AUDIO_FIFO)
    except FileNotFoundError:
        pass


def cleanup():
    global g, bridge
    stop.set()
    audio_fifo_cleanup()
    if bridge is not None:
        try:
            bridge.terminate()
            bridge.wait(timeout=3)
        except Exception:
            try:
                bridge.kill()
            except Exception:
                pass
        bridge = None
    if g is not None:
        try:
            g.terminate(force=True)
        except Exception:
            pass
        g = None
    for p in (QMP_SOCKET, UART3_SOCKET, SER5_SOCKET, GDB_SOCKET, USB_SOCKET, PIDFILE):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass


def main():
    global g, bridge
    if not os.path.exists(BRIDGE):
        raise SystemExit('build rawplay/out/rawplay-linux first')
    if not os.path.exists(STICK):
        raise SystemExit('build usb.img (make stick) first: the unit runs rawplay off the stick')
    built = os.path.join(JB, 'out', 'rawplay')
    if os.path.exists(built) and os.path.getmtime(STICK) < os.path.getmtime(built):
        status('usb.img is older than out/rawplay; make stick to run the current player')
    for p in (QMP_SOCKET, UART3_SOCKET, SER5_SOCKET, GDB_SOCKET, USB_SOCKET, PIDFILE):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass
    with open(PIDFILE, 'w') as f:
        f.write(str(os.getpid()))
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    extra = ['-qmp', f'unix:{QMP_SOCKET},server=on,wait=off',
             '-drive', f'if=none,id=stick,format=raw,read-only=on,file={STICK}',
             '-chardev', f'socket,id=liviusb,path={USB_SOCKET},server=on,wait=off']
    if DISPLAY_MODE == 'vnc':
        extra += ['-display', f'vnc={VNC}']
    elif DISPLAY_MODE == 'none':
        extra += ['-display', 'none']
    else:
        extra += ['-display', 'gtk']
    if AUDIO == 'wav':
        extra += ['-audiodev', f'wav,id=snd,path={WAV}',
                  '-global', 'wm8580.audiodev=snd']
    elif AUDIO == 'pacat':
        audio_fifo_player()
        extra += ['-audiodev', f'wav,id=snd,path={AUDIO_FIFO}',
                  '-global', 'wm8580.audiodev=snd']
    elif AUDIO == 'pipewire':
        extra += ['-audiodev', 'pipewire,id=snd', '-global', 'wm8580.audiodev=snd']
    else:
        extra += ['-audiodev', 'pa,id=snd', '-global', 'wm8580.audiodev=snd']

    status('resuming the started snapshot')
    g = resume('started', extra, log=open(CONSOLE, 'w', buffering=1))
    q = qmp.Qmp(QMP_SOCKET)
    time.sleep(2)

    status('plugging qemu usb-livi')
    r = q.cmd('device_add', driver='usb-livi', id='liviusb0', drive='stick',
              chardev='liviusb', chunked=True)
    if 'error' in r:
        raise SystemExit(f'usb-livi hotplug failed: {r}')
    time.sleep(3)
    hmi.run(g, hmi.USB_START, timeout=60)

    status('starting the appliance <-> usb-livi bridge')
    bridge = subprocess.Popen([BRIDGE, '--bridge', USB_SOCKET],
                              stdout=open(BRIDGE_LOG, 'w'),
                              stderr=subprocess.STDOUT)
    time.sleep(1)

    status('starting the unit player off the stick')
    type_line(g, f'sh {STICK_RAWPLAY} 0 --direct')
    threading.Thread(target=drain, daemon=True).start()

    status(f'emulator up; display={DISPLAY_MODE} audio={AUDIO}')
    status(f'console {CONSOLE}')
    status(f'bridge  {BRIDGE_LOG}')
    while not stop.wait(1):
        if bridge.poll() is not None:
            status('bridge exited; stopping')
            break
    cleanup()
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    finally:
        cleanup()
