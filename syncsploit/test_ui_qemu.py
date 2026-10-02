#!/usr/bin/env python3
"""the syncsploit ui in the emulator: status, single instance, cancel, install.

resumes the 'started' snapshot the test bench uses (qemu/tests/mksnap.py), uploads
`out/syncsploit` to /tmp and drives it with clicks over qmp, taking a screenshot after
every step and checking the lamp colour on the panel against what the unit's nand says:

  - no /packages/system/override/hmi_startup.sh: red lamp, "Not jailbroken"
  - the hook present as this build's: green lamp, "Jailbroken"
  - a different hmi_startup.sh: amber lamp, "Different jailbreak installed"
  - a second launch while the ui is up exits without touching the screen
  - cancel restores the hmi's screen and resumes it
  - jailbreak installs jailbreak/hmi_startup.sh byte for byte and restarts

the screenshots land in qemu/tests/out-ui/ (sheet.png is the contact sheet), and the guest is
left to reboot after the install step, so the run ends there.

    python3 syncsploit/test_ui_qemu.py [--keep-sheet]
"""
import argparse
import functools
import hashlib
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
ICC2 = os.environ.get('ICC2_DIR') or os.path.normpath(os.path.join(REPO, '..', '..', '..', 'ICC2'))
QEMU = os.path.join(ICC2, 'qemu')
HOOK = os.path.join(REPO, 'jailbreak', 'hmi_startup.sh')
UI = os.path.join(REPO, 'out', 'syncsploit')
OUT = os.path.join(QEMU, 'tests', 'out-ui')

sys.path.insert(0, QEMU)
import qmp
import upload
import v850
from guest import GDB_SOCKET, QMP_SOCKET, SER5_SOCKET, UART3_SOCKET, resume
from PIL import Image, ImageDraw

SCREEN = (800, 480)
THUMB = (400, 240)
COLUMNS = 3
LABEL_HEIGHT = 20
SETTLE = 2.5
ABS_MAX = 32767
LAMP = (400, 183)           # syncsploit/syncsploit.c LAMP_CY, dead centre of the status lamp
JAILBREAK_BTN = (272, 369)
CANCEL_BTN = (528, 369)


class Qmp(qmp.Qmp):
    def shot(self, name):
        path = os.path.join(OUT, name + '.ppm')
        self.cmd('screendump', filename=path)
        return Image.open(path).convert('RGB')

    def click(self, x, y, hold=0.3):
        pos = [{'type': 'abs', 'data': {'axis': a, 'value': int(v * ABS_MAX / (size - 1))}}
               for a, v, size in (('x', x, SCREEN[0]), ('y', y, SCREEN[1]))]
        self.cmd('input-send-event', events=pos)
        for down in (True, False):
            self.cmd('input-send-event',
                     events=[{'type': 'btn', 'data': {'button': 'left', 'down': down}}])
            time.sleep(hold)


def lamp_colour(im):
    """which channel dominates the lamp: 'red', 'green', 'amber' or '?'"""
    r, g, b = im.getpixel(LAMP)
    if g > r:
        return 'green'
    if g > b * 2:
        return 'amber'
    if r > g * 2:
        return 'red'
    return '?'


class Bench:
    def __init__(self, log):
        self.lines = []
        self.shots = []
        self.g = resume('started',
                        ['-display', 'none', '-qmp', f'unix:{QMP_SOCKET},server=on,wait=off'],
                        log=open(os.path.join(OUT, 'console.log'), 'w'))
        self.micro = v850.V850(v850.connect(UART3_SOCKET), self.lines.append)
        self.micro.uploader = functools.partial(upload.upload, GDB_SOCKET)
        self.q = Qmp(QMP_SOCKET)
        time.sleep(SETTLE)

    def sh(self, line, wait=1.5):
        self.lines.clear()
        v850.command(self.micro, 'sh ' + line)
        time.sleep(wait)
        return '\n'.join(self.lines)

    def step(self, name):
        im = self.q.shot(name)
        self.shots.append((name, im))
        return im

    def close(self):
        self.g.terminate(force=True)
        for path in (QMP_SOCKET, UART3_SOCKET, SER5_SOCKET, GDB_SOCKET):
            if os.path.exists(path):
                os.unlink(path)


def sheet(shots):
    rows = (len(shots) + COLUMNS - 1) // COLUMNS
    sheet = Image.new('RGB', (THUMB[0] * COLUMNS, (THUMB[1] + LABEL_HEIGHT) * rows), 'black')
    draw = ImageDraw.Draw(sheet)
    for i, (label, im) in enumerate(shots):
        x, y = (i % COLUMNS) * THUMB[0], (i // COLUMNS) * (THUMB[1] + LABEL_HEIGHT)
        sheet.paste(im.resize(THUMB), (x, y + LABEL_HEIGHT))
        draw.text((x + 4, y + 4), label, fill='white')
    sheet.save(os.path.join(OUT, 'sheet.png'))
    shots[-1][1].save(os.path.join(OUT, 'last.png'))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--keep-sheet', action='store_true', help='keep the screenshots')
    args = ap.parse_args()
    for need in (UI, HOOK, os.path.join(QEMU, 'snap.qcow2')):
        if not os.path.exists(need):
            sys.exit('missing %s (build out/syncsploit, run qemu/tests/mksnap.py)' % need)
    os.makedirs(OUT, exist_ok=True)
    hook_md5 = hashlib.md5(open(HOOK, 'rb').read()).hexdigest()
    results = []
    b = Bench(sys.stdout)
    try:
        b.step('start (hmi)')

        print('stock unit: removing the hook, then starting the ui')
        print(b.sh('rm -f /packages/system/override/hmi_startup.sh'))
        v850.command(b.micro, 'upload ' + UI)
        time.sleep(2)
        # in the background, so the console shell stays free for the checks below (the
        # payload path runs it foreground inside the connector's own shell)
        b.sh('/tmp/syncsploit &', wait=2)
        im = b.step('not jailbroken')
        results.append(('no hook on the unit -> red lamp', lamp_colour(im) == 'red'))

        print('second launch on top of the first')
        out = b.sh('/tmp/syncsploit', wait=1)
        results.append(('second instance refuses to open',
                        'already running' in out.replace('\\r', '').replace('\r', '')))
        im = b.step('second instance ignored')
        results.append(('second instance left the panel alone', lamp_colour(im) == 'red'))

        print('cancel: the hmi gets the screen back')
        b.q.click(*CANCEL_BTN)
        time.sleep(SETTLE)
        b.step('cancelled (hmi)')
        results.append(('cancel killed the ui (lamp gone)',
                        lamp_colour(b.q.shot('cancelled-check')) != 'red'))

        print('a different hook on the unit')
        print(b.sh('mkdir -p /packages/system/override; echo different > '
                   '/packages/system/override/hmi_startup.sh', wait=2))
        b.sh('/tmp/syncsploit &', wait=2)
        im = b.step('different jailbreak')
        results.append(('other hook -> amber lamp', lamp_colour(im) == 'amber'))

        print('jailbreak: install and restart')
        b.q.click(*JAILBREAK_BTN)
        time.sleep(2)
        im = b.step('jailbreak successful')
        results.append(('jailbreak button -> green lamp', lamp_colour(im) == 'green'))
        md5 = b.sh('md5sum /packages/system/override/hmi_startup.sh').replace('\r', '')
        print('md5 on the unit:', md5.strip())
        results.append(('installed hook matches jailbreak/hmi_startup.sh',
                        hook_md5 in md5.lower()))
        time.sleep(6)
        b.step('restarting')
    finally:
        b.close()
    sheet(b.shots)
    print('\n=== results')
    ok = True
    for name, passed in results:
        print('  %-52s %s' % (name, 'PASS' if passed else 'FAIL'))
        ok = ok and passed
    print('\nsheet', os.path.join(OUT, 'sheet.png'))
    if not args.keep_sheet:
        for f in os.listdir(OUT):
            if f.endswith('.ppm'):
                os.unlink(os.path.join(OUT, f))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
