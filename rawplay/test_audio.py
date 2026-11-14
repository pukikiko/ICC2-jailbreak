#!/usr/bin/env python3
# host-side audio loopback over the unix-socket transport, no usb and no emulator:
#
#     make out/rawplay && make -C rawplay rawlink linux-client
#     python3 rawplay/test_audio.py
#
# rawlink sends its built-in 1 kHz test tone in the new LR_AUDIO messages while
# rawplay-linux sends a 440 Hz u8/8k microphone tone back as LI_AUDIO. the test
# checks both PCM streams, the video stream, the 20 ms chunking and the exact
# wire formats.
import os, signal, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
RAWLINK = os.path.join(HERE, 'out', 'rawlink')
CLIENT = os.path.join(HERE, 'out', 'rawplay-linux')
SECONDS = 6


def main():
    for p in (RAWLINK, CLIENT):
        if not os.path.exists(p):
            raise SystemExit(f'missing {p}; build with make out/rawplay && '
                             f'make -C rawplay rawlink linux-client')
    tmp = tempfile.mkdtemp(prefix='rawplay-audio-')
    sock = os.path.join(tmp, 'link.sock')
    rx = os.path.join(tmp, 'rx.s16')
    mic_in = os.path.join(tmp, 'mic-tx.u8')
    mic_out = os.path.join(tmp, 'mic-rx.u8')
    cli_log = os.path.join(tmp, 'client.log')
    host_log = os.path.join(tmp, 'host.log')

    cli = subprocess.Popen(
        [CLIENT, '--sock', sock, '--headless', '--audio', '--audio-dump', rx,
         '--mic-test', '--mic-dump', mic_in, '--seconds', str(SECONDS), '--stats'],
        stdout=open(cli_log, 'w'), stderr=subprocess.STDOUT,
        env={**os.environ, 'SDL_AUDIODRIVER': 'dummy'})
    time.sleep(0.4)
    host = subprocess.Popen(
        [RAWLINK, 'stream', sock, '--test', '--audio-test', '--audio-out', mic_out,
         '--seconds', str(SECONDS), '--stats'],
        stdout=open(host_log, 'w'), stderr=subprocess.STDOUT)
    host.wait(timeout=SECONDS + 10)
    cli.wait(timeout=SECONDS + 10)

    ok = True
    def check(name, cond, detail=''):
        nonlocal ok
        print(('PASS ' if cond else 'FAIL ') + name + (': ' + detail if detail else ''))
        ok = ok and cond

    rx_data = open(rx, 'rb').read() if os.path.exists(rx) else b''
    mic_tx = open(mic_in, 'rb').read() if os.path.exists(mic_in) else b''
    mic_rx = open(mic_out, 'rb').read() if os.path.exists(mic_out) else b''

    check('downstream is exact 20 ms s16le chunks',
          len(rx_data) > 0 and len(rx_data) % 3528 == 0, f'{len(rx_data)} bytes')
    if rx_data:
        import struct
        s = struct.unpack('<%dh' % (len(rx_data) // 2), rx_data)
        peak = max(abs(x) for x in s)
        zc = sum(1 for a, b in zip(s, s[1:]) if (a < 0) != (b < 0))
        hz = zc / 2 / (len(rx_data) / 176400)
        check('downstream tone is present', peak > 1000, f'peak {peak}')
        check('downstream is the 1 kHz test tone', 900 <= hz <= 1100, f'{hz:.0f} Hz')

    check('microphone is exact 20 ms u8 chunks',
          len(mic_rx) > 0 and len(mic_rx) % 160 == 0, f'{len(mic_rx)} bytes')
    check('microphone loopback is byte-identical', mic_tx == mic_rx,
          f'{len(mic_tx)} vs {len(mic_rx)}')
    if mic_rx:
        zc = sum(1 for a, b in zip(mic_rx, mic_rx[1:])
                 if (a < 128) != (b < 128))
        hz = zc / 2 / (len(mic_rx) / 8000)
        check('microphone is the 440 Hz test tone', 400 <= hz <= 480, f'{hz:.0f} Hz')

    log = open(cli_log).read()
    check('video kept flowing', 'frames,' in log and '0 frames' not in log)
    if not ok:
        print('\nclient log:\n' + log)
        print('\nhost log:\n' + open(host_log).read())
    return 0 if ok else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except subprocess.TimeoutExpired:
        print('FAIL timeout')
        sys.exit(1)
