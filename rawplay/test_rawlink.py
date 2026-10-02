#!/usr/bin/env python3
# protocol and robustness tests for rawplay/rawlink.c, no emulator needed:
#
#     make -C rawplay rawlink && python3 rawplay/test_rawlink.py
#
# what it proves:
#   * the functionfs descriptor and string blocks are the exact bytes the unit has always
#     enumerated (frozen from the removed livi/usbgadget.py), so the device is unchanged;
#   * against a fake unit, rawlink's LR stream carries ffmpeg's rgb565le output byte for
#     byte: the expected MODE payload, the header layout, and the frame bytes
#     (deterministic source, window 0 so nothing drops);
#   * hostile input cannot kill it: garbage and truncated LI, a unit that goes away
#     mid-stream and comes back, a socket that appears late, an ffmpeg that dies, a huge
#     burst of junk, and SIGTERM (which should end it cleanly);
#   * the icc/swc buttons with a livi binding tap their x11 key on the press edge only,
#     and unmapped buttons are left alone.
import os
import random
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RAWLINK = os.path.join(HERE, 'out', 'rawlink')

# frozen from the removed livi/usbgadget.py: the configfs/functionfs device the unit has
# always enumerated. rawlink must keep writing these exact bytes.
DESCRIPTORS = bytes.fromhex(
    '010000003e00000003000000030000000904000002ffffff00070583024000000705040240'
    '00000904000002ffffff000705830200020007050402000200')
STRINGS = bytes.fromhex('02000000100000000000000000000000')

TMP = tempfile.mkdtemp(prefix='rawlink-test-')
PASS, FAIL = [], []


def check(name, ok, detail=''):
    if ok:
        PASS.append(name)
        print(f'  PASS {name}')
    else:
        FAIL.append(name)
        print(f'  FAIL {name}: {detail}')


class FakeUnit(threading.Thread):
    """a unix server standing in for the car: parses LR, records every message, acks
    frames, and can misbehave in the ways the tests need."""

    def __init__(self, path, ack=True, ready=True, garbage=b'', close_every=0,
                 touch=None, touch_burst=b'', knob=None, buttons=(), junk_after=0):
        super().__init__(daemon=True)
        self.path = path
        self.ack = ack
        self.ready = ready
        self.garbage = garbage
        self.close_every = close_every
        self.touch = touch
        self.touch_burst = touch_burst
        self.knob = knob
        self.buttons = buttons
        self.junk_after = junk_after
        self.messages = []              # (type, seq, len, ts, payload)
        self.frames = 0
        self.conns = 0
        self.stop_flag = threading.Event()
        self.listening = threading.Event()
        self.srv = None

    def start_server(self):
        self.start()
        self.listening.wait(2)

    def stop(self):
        self.stop_flag.set()
        try:
            if self.srv:
                self.srv.close()
        except OSError:
            pass
        self.join(timeout=2)

    def run(self):
        self.srv = socket.socket(socket.AF_UNIX)
        if os.path.exists(self.path):
            os.unlink(self.path)
        self.srv.bind(self.path)
        self.srv.listen(2)
        self.srv.settimeout(0.2)
        self.listening.set()
        while not self.stop_flag.is_set():
            try:
                conn, _ = self.srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self.conns += 1
            try:
                if self.garbage:
                    conn.sendall(self.garbage)
                if self.ready:
                    conn.sendall(b'LI\x05')
                if self.touch:
                    conn.sendall(b'LI\x01' + struct.pack('<HHB', *self.touch))
                if self.touch_burst:
                    conn.sendall(self.touch_burst)
                if self.knob:
                    conn.sendall(b'LI\x02' + bytes([self.knob & 0xff]))
                for bit, down in self.buttons:
                    conn.sendall(b'LI\x03' + bytes([bit, down]))
                self.serve(conn)
            except OSError:
                pass
            finally:
                try:
                    conn.close()
                except OSError:
                    pass

    def serve(self, conn):
        buf = b''
        conn.settimeout(0.2)
        while not self.stop_flag.is_set():
            try:
                data = conn.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                return
            if not data:
                return
            buf += data
            buf, gone = self.parse(buf, conn)
            if gone:
                return

    def parse(self, buf, conn):
        """returns the unparsed tail and whether the connection should close"""
        while True:
            i = buf.find(b'LR')
            if i < 0:
                return (buf[-1:] if buf.endswith(b'L') else b''), False
            if len(buf) - i < 16:
                return buf[i:], False
            mtype, flags, ln, seq, ts = struct.unpack_from('<BBIII', buf, i + 2)
            if mtype not in (1, 2) or flags != 0 or ln > (1 << 20):
                buf = buf[i + 2:]
                continue
            # every LR message is a whole number of max packets: MODE is header+payload+pad
            # (512 total, payload at 16), FRAME is the header padded to 512 then the payload
            if mtype == 1:
                if len(buf) - i < 512:
                    return buf[i:], False
                payload = bytes(buf[i + 16:i + 16 + ln])
                total = 512
            else:
                if len(buf) - i < 512 + ln:
                    return buf[i:], False
                payload = bytes(buf[i + 512:i + 512 + ln])
                total = 512 + ln
            self.messages.append((mtype, seq, ln, ts, payload))
            if mtype == 2:
                self.frames += 1
                if self.ack:
                    try:
                        conn.sendall(b'LI\x08' + struct.pack('<I', seq) + b'\x01')
                    except OSError:
                        return b'', True
                if self.junk_after and self.frames == self.junk_after:
                    try:
                        conn.sendall(bytes(random.getrandbits(8) for _ in range(4096))
                                     + b'LI\x01\x00')
                    except OSError:
                        return b'', True
                if self.close_every and self.frames % self.close_every == 0:
                    return b'', True
            buf = buf[i + total:]

    def mode_payload(self):
        for m in self.messages:
            if m[0] == 1:
                return m[4]
        return None

    def frame_payloads(self):
        return {m[1]: m[4] for m in self.messages if m[0] == 2}


def run_c(args, env=None, timeout=10, signal_after=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    p = subprocess.Popen([RAWLINK, 'stream'] + args, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, env=e)
    if signal_after is not None:
        time.sleep(signal_after)
        p.send_signal(signal.SIGTERM)
    try:
        out, err = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        out, err = p.communicate()
        return p.returncode, out, err, True
    return p.returncode, out, err, False


def test_descriptors():
    print('descriptors')
    r = subprocess.run([RAWLINK, 'gadget', '--dump-descriptors'],
                       capture_output=True, text=True, check=True)
    got = dict(line.split(' ', 1) for line in r.stdout.splitlines())
    check('descriptors match the frozen device bytes',
          got['descriptors'] == DESCRIPTORS.hex(), got.get('descriptors'))
    check('strings match the frozen device bytes',
          got['strings'] == STRINGS.hex(), got.get('strings'))


def make_source():
    path = os.path.join(TMP, 'source.y4m')
    if not os.path.exists(path):
        subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y',
                        '-f', 'lavfi', '-i', 'testsrc2=s=64x48:r=10', '-frames:v', '12',
                        '-pix_fmt', 'yuv420p', path], check=True)
    return path


def reference_frames(src, width, height):
    """ffmpeg's own rgb565le output for the deterministic source: the exact bytes the
    sender must put in FRAME payloads."""
    r = subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-i', src,
                        '-pix_fmt', 'rgb565le', '-f', 'rawvideo', '-'],
                       capture_output=True, check=True)
    flen = width * height * 2
    return [r.stdout[i:i + flen] for i in range(0, len(r.stdout), flen)]


def test_protocol():
    print('protocol bytes vs ffmpeg (64x48, deterministic file, window 0)')
    src = make_source()
    sock = os.path.join(TMP, 'c.sock')
    args = ['--usb', '--file', src, '--fps', '10', '--width', '64', '--height', '48',
            '--window', '0', '--seconds', '2']

    unit = FakeUnit(sock)
    unit.start_server()
    rc, out, err, timedout = run_c([sock] + args, timeout=15)
    unit.stop()
    check('rawlink stream exited cleanly', rc == 0 and not timedout,
          f'rc={rc} timedout={timedout} err={err[-200:]!r}')

    mode = unit.mode_payload()
    check('MODE format is chunked rgb565', mode == struct.pack('<4I', 64, 48, 128, 0x101),
          str(mode))

    mode_msg = next((m for m in unit.messages if m[0] == 1), None)
    check('MODE header (magic/type/flags/len) right', mode_msg and mode_msg[:3] == (1, 0, 16),
          str(mode_msg))

    frames = unit.frame_payloads()
    got = [frames[s] for s in sorted(frames)]
    check('enough frames to compare', len(got) >= 10, f'got={len(got)}')
    check('frame seqs strictly increasing', all(b > a for a, b in zip(sorted(frames),
                                                                      sorted(frames)[1:])))
    check('all frame lengths right', all(len(v) == 64 * 48 * 2 for v in got))
    # the source loops (-stream_loop -1) and the sender drops source frames while one is
    # still going out, so walk forward through the reference, wrapping at the loop
    expected = reference_frames(src, 64, 48)
    e, matched = 0, 0
    for payload in got:
        for _ in range(len(expected)):
            hit = expected[e] == payload
            e = (e + 1) % len(expected)
            if hit:
                matched += 1
                break
    check('every frame payload is ffmpeg rgb565le byte-for-byte', matched == len(got),
          f'{matched}/{len(got)} matched')


def test_no_chunked():
    print('--no-chunked mode word')
    sock = os.path.join(TMP, 'nc.sock')
    unit = FakeUnit(sock)
    unit.start_server()
    run_c([sock, '--test', '--width', '32', '--height', '24', '--fps', '10',
           '--seconds', '1', '--no-chunked'], timeout=10)
    unit.stop()
    check('format word is plain rgb565 without bit 8',
          unit.mode_payload() == struct.pack('<4I', 32, 24, 64, 1),
          str(unit.mode_payload()))


def test_hostile_li():
    print('hostile LI data')
    sock = os.path.join(TMP, 'bad.sock')
    junk = (b'\x00' * 5000 + b'LI' + b'\x01\x02' + b'LI\xff\xff\xff\xff'
            + b'L' + bytes(random.getrandbits(8) for _ in range(1000)))
    unit = FakeUnit(sock, garbage=junk, junk_after=3)
    unit.start_server()
    rc, out, err, timedout = run_c([sock, '--test', '--width', '32', '--height', '24',
                                    '--fps', '15', '--seconds', '2', '--stats'],
                                   timeout=10)
    unit.stop()
    check('survives garbage and truncated LI', rc == 0 and not timedout and unit.frames > 8,
          f'rc={rc} frames={unit.frames} err={err[-200:]!r}')


def test_reconnect():
    print('unit disconnect and reconnect')
    sock = os.path.join(TMP, 're.sock')
    unit = FakeUnit(sock, close_every=4)
    unit.start_server()
    rc, out, err, timedout = run_c([sock, '--test', '--width', '32', '--height', '24',
                                    '--fps', '15', '--seconds', '3'], timeout=12)
    unit.stop()
    check('keeps streaming across disconnects',
          rc == 0 and not timedout and unit.conns >= 2 and unit.frames > 8,
          f'rc={rc} conns={unit.conns} frames={unit.frames}')
    check('logged the reconnect', b'went away, reconnecting' in err and b' up' in err,
          err[-300:])


def test_late_socket():
    print('socket appears late')
    sock = os.path.join(TMP, 'late.sock')
    for stale in (sock,):
        if os.path.exists(stale):
            os.unlink(stale)
    p = subprocess.Popen([RAWLINK, 'stream', sock, '--test', '--width', '32',
                          '--height', '24', '--fps', '15', '--seconds', '3'],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.8)
    alive = p.poll() is None
    unit = FakeUnit(sock)
    unit.start_server()
    out, err = p.communicate(timeout=10)
    unit.stop()
    check('waits for a socket that is not there yet',
          alive and p.returncode == 0 and unit.frames > 4 and b'waiting for' in err,
          f'alive={alive} rc={p.returncode} frames={unit.frames} err={err[-200:]!r}')


def test_ffmpeg_death():
    print('ffmpeg pipe closes')
    bindir = os.path.join(TMP, 'bin')
    os.makedirs(bindir, exist_ok=True)
    fake = os.path.join(bindir, 'ffmpeg')
    with open(fake, 'w') as f:
        f.write('#!/bin/sh\n'
                'for i in 1 2 3 4 5; do\n'
                '  dd if=/dev/zero bs=$((32*24*2)) count=1 2>/dev/null\n'
                '  sleep 0.05\n'
                'done\n'
                'exit 0\n')
    os.chmod(fake, 0o755)
    sock = os.path.join(TMP, 'ff.sock')
    unit = FakeUnit(sock)
    unit.start_server()
    rc, out, err, timedout = run_c([sock, '--test', '--width', '32', '--height', '24',
                                    '--seconds', '2'],
                                   env={'PATH': bindir + os.pathsep + os.environ['PATH']},
                                   timeout=10)
    unit.stop()
    check('restarts ffmpeg and keeps streaming',
          rc == 0 and not timedout and b'restarting ffmpeg' in err and unit.frames > 8,
          f'rc={rc} frames={unit.frames} err={err[-300:]!r}')


def test_signals():
    print('signals')
    sock = os.path.join(TMP, 'term.sock')
    unit = FakeUnit(sock)
    unit.start_server()
    p = subprocess.Popen([RAWLINK, 'stream', sock, '--test', '--width', '32',
                          '--height', '24', '--fps', '15', '--seconds', '30'],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.8)
    p.send_signal(signal.SIGHUP)
    time.sleep(0.4)
    alive = p.poll() is None
    p.send_signal(signal.SIGTERM)
    try:
        out, err = p.communicate(timeout=5)
        timedout = False
    except subprocess.TimeoutExpired:
        p.kill()
        out, err = p.communicate()
        timedout = True
    unit.stop()
    check('SIGHUP is ignored, SIGTERM exits cleanly',
          alive and p.returncode == 0 and not timedout and b'rawstream:' in out,
          f'alive={alive} rc={p.returncode} timedout={timedout} out={out[-200:]!r}')


def test_events():
    print('events log')
    sock = os.path.join(TMP, 'ev.sock')
    events = os.path.join(TMP, 'events.log')
    if os.path.exists(events):
        os.unlink(events)
    unit = FakeUnit(sock, touch=(123, 45, 1), knob=1)
    unit.start_server()
    rc, out, err, timedout = run_c([sock, '--test', '--width', '32', '--height', '24',
                                    '--fps', '15', '--seconds', '1.5', '--events', events,
                                    '--no-inject'], timeout=10)
    unit.stop()
    text = open(events).read() if os.path.exists(events) else ''
    check('touch and drawn events are written',
          rc == 0 and 'touch 123 45 1' in text and 'drawn ' in text,
          f'rc={rc} events={text!r}')


def test_button_keys():
    print('panel buttons tap their livi keys')
    # the icc/swc bits with a livi binding, plus one that has none: the mapped bits log the
    # x11 keysym they tap on the press edge, the release and the unmapped bit must not
    keys = {25: '0xff08', 28: '0x068', 29: '0x062', 30: '0x06e', 46: '0x076', 48: '0x06e'}
    seq = []
    for bit in list(keys) + [27]:
        seq += [(bit, 1), (bit, 0)]
    sock = os.path.join(TMP, 'btn.sock')
    events = os.path.join(TMP, 'btn.log')
    if os.path.exists(events):
        os.unlink(events)
    unit = FakeUnit(sock, buttons=seq)
    unit.start_server()
    rc, out, err, timedout = run_c([sock, '--test', '--width', '32', '--height', '24',
                                    '--fps', '15', '--seconds', '1.5', '--events', events,
                                    '--no-inject'], timeout=10)
    unit.stop()
    text = open(events).read() if os.path.exists(events) else ''
    ok = rc == 0 and not timedout
    for bit, key in keys.items():
        ok = ok and f'button {bit} 1 key {key}' in text
        ok = ok and f'button {bit} 0 key' not in text
    ok = ok and 'button 27 1\n' in text and 'button 27 1 key' not in text
    check('buttons 25/28/29/30/46/48 tap on the press edge only', ok,
          f'rc={rc} timedout={timedout} events={text!r}')


def test_touch_replay():
    print('queued touch burst coalesced')
    # the touch driver hands a fresh reader the samples from before it opened as one run,
    # delivered with the next touch; only the newest of the run is a real position
    burst = b''.join(b'LI\x01' + struct.pack('<HHB', x, 45, 1) for x in range(0, 120, 10))
    args = ['--test', '--width', '32', '--height', '24', '--fps', '15', '--seconds', '1.5',
            '--no-inject']
    sock = os.path.join(TMP, 'replay.sock')
    events = os.path.join(TMP, 'replay.log')
    if os.path.exists(events):
        os.unlink(events)
    unit = FakeUnit(sock, touch_burst=burst)
    unit.start_server()
    rc, out, err, timedout = run_c([sock] + args + ['--events', events], timeout=10)
    unit.stop()
    text = open(events).read() if os.path.exists(events) else ''
    touches = [line.split() for line in text.splitlines()
               if line.split()[1:2] == ['touch']]
    check('only the newest sample of a touch run is injected',
          rc == 0 and not timedout and len(touches) == 1
          and touches[0][2:5] == ['110', '45', '1'],
          f'rc={rc} timedout={timedout} touches={touches[:3]}')


def test_junk_flood():
    print('junk flood')
    sock = os.path.join(TMP, 'flood.sock')
    unit = FakeUnit(sock, ack=False, garbage=os.urandom(1 << 20))
    unit.start_server()
    rc, out, err, timedout = run_c([sock, '--test', '--width', '32', '--height', '24',
                                    '--fps', '15', '--seconds', '2'],
                                   timeout=10)
    unit.stop()
    check('1 MB of junk does not kill it', rc == 0 and not timedout,
          f'rc={rc} timedout={timedout}')


def test_gadget_bridge():
    print('gadget bridge against fifos (no usb device controller)')
    r = subprocess.run(['make', '-s', '-C', HERE, 'test-gadget'],
                       capture_output=True, text=True)
    if r.returncode != 0:
        check('test_gadget builds', False, r.stderr[-300:])
        return
    r = subprocess.run([os.path.join(HERE, 'out', 'test_gadget')],
                       capture_output=True, text=True, timeout=60)
    check('maxpacket framing, ep2 forwarding, enable cycle, client loss',
          r.returncode == 0, (r.stdout + r.stderr)[-300:])


def main():
    if not os.path.exists(RAWLINK):
        print(f'rawlink not built: run make -C {HERE} rawlink')
        return 2
    test_descriptors()
    test_gadget_bridge()
    test_protocol()
    test_no_chunked()
    test_hostile_li()
    test_reconnect()
    test_late_socket()
    test_ffmpeg_death()
    test_signals()
    test_events()
    test_button_keys()
    test_touch_replay()
    test_junk_flood()
    print(f'\n{len(PASS)} passed, {len(FAIL)} failed')
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
