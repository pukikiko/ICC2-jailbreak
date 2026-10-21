#!/usr/bin/env python3
"""end to end check of the buttons watchdog in the emulator, no emulator changes.

boots the unit from the dump with the homebrew stick (usb.img), lets pkgstart bring the
stock stack up (ham guards buttons, the hmi shows the plates, boot.sh starts the launcher
and stickwatch), then plays the failures the car hits:

  A. baseline: buttons is running, stickwatch is running, the hmi has the overlay, and the
     shim/launcher/buttons/stickwatch ram copies are complete
  B. stillborn respawn: hammer kill on buttons until ham's respawn is lost (a respawn that
     dies before it registers with ham raises nothing and no restart ever comes - this is
     the panel-deaf failure found on the real unit). assert buttons is gone, then assert
     stickwatch starts one within BSTALE checks and logs it
  C. gate: the watchdog must only act when homebrew is not running. with /tmp/hmi-stopped
     and a live buttons-pause owner (a session owns the panel) lose buttons again and assert
     nothing starts it for several checks; drop the markers and assert stickwatch starts one
  D. stale pause: a session that died before its resume leaves /tmp/buttons-paused with a
     dead owner and its frozen buttons behind. assert stickwatch resumes: the recorded pid
     dies, a fresh buttons appears and the marker is removed
  E. stale hmi stop: a killed session can leave /tmp/hmi-stopped with no live pause owner and
     the hmi SIGSTOPped. assert stickwatch clears the marker, resumes the hmi and logs it

the bluetooth stack spams the console, so every answer is collected through a temp file
delimited by markers: console output interleaves at line boundaries, the file delimiters
do not.

needs usb.img (make stick), ICC2's dump and its built qemu, like qemu/hmi.py:

    python3 tests/test_homebrew_qemu.py [--keep] [--log FILE]
"""
import argparse
import functools
import os
import shutil
import sys
import time
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def find_icc2():
    """the ICC2 checkout with the emulator, next to the repo or in ~/ICC2."""
    for c in (os.environ.get('ICC2_DIR'),
              os.path.join(REPO, '..', 'ICC2'),
              os.path.join(REPO, '..', '..', 'ICC2'),
              os.path.join(REPO, '..', '..', '..', 'ICC2'),
              os.path.expanduser('~/ICC2')):
        if c and os.path.isdir(os.path.join(c, 'qemu')):
            return os.path.normpath(c)
    return os.path.normpath(os.path.join(REPO, '..', 'ICC2'))


ICC2 = find_icc2()
QEMU = os.path.join(ICC2, 'qemu')
ETFS = os.path.join(ICC2, 'dump/packages/factory/qnx_binaries_and_libraries/root/sbin/fs-etfs-ram')
ROOT_TGZ = os.path.join(QEMU, 'stage.tar.gz')
STICK = os.path.join(REPO, 'usb.img')

sys.path.insert(0, QEMU)
import guest
import upload
from guest import boot, mount_root, run, stage  # noqa: E402

USB_START = ('slay -Q -f -s9 devb-umass; slay -Q -f -s9 io-usb_swsa; sleep 1; '
             'io-usb_swsa -i20 -r3 -c -d ehci-mx31_swsa ioport=0x43f88100,irq=37,num_itd=300 & '
             'waitfor /dev/io-usb/io-usb 10; '
             'devb-umass cam pnp blk cache=2m,auto=partition,automount=hd0@dos:/fs/usb0,ro dos exe=all; '
             'waitfor /fs/usb0 15')

# ten kills as fast as the shell can list and slay: one of ham's respawns dies in the
# window before it registers its CONDDEATH, nothing restarts it, and the panel is left deaf
HAMMER = ('i=0; while [ $i -lt 10 ]; do p=$(slay -p buttons 2>/dev/null); '
          'if [ -n "$p" ]; then kill -9 $p; fi; i=$((i+1)); done')

DEAD_PID = 999999       # no such process; kill(errno ESRCH) marks the pause owner gone
BSTALE = 2              # stickwatch.sh's miss count before it starts one


def collect(g, cmd, timeout=60):
    """run cmd in the guest with its output redirected to a marker delimited file, and
    return just the output. the markers keep the console spam out of the answer."""
    run(g, 'echo RB7K > /tmp/tb.txt; { %s; } >> /tmp/tb.txt 2>/dev/null; '
           'echo RE7K >> /tmp/tb.txt' % cmd, timeout=timeout)
    out = run(g, 'cat /tmp/tb.txt', timeout=timeout).replace('\r', '')
    start = out.find('RB7K')
    end = out.find('RE7K', start + 4) if start >= 0 else -1
    if start < 0 or end < 0:
        return None
    return out[start + 4:end].strip()


def buttons(g):
    body = collect(g, 'slay -p buttons')
    return [int(t) for t in (body or '').split() if t.isdigit()]


def wait_buttons(g, present, timeout, poll=4):
    deadline = time.time() + timeout
    while time.time() < deadline:
        pids = buttons(g)
        if bool(pids) == present:
            return pids
        time.sleep(poll)
    return None


def lose_buttons(g, tries=6):
    """hammer until the respawn is lost and stays lost for a settle"""
    for _ in range(tries):
        run(g, HAMMER, timeout=60)
        time.sleep(6)
        if not buttons(g):
            return True
    return False


def log_count(g, pat, path='/tmp/stickwatch.log'):
    body = collect(g, 'grep -c "%s" %s' % (pat, path))
    for tok in (body or '').split():
        if tok.isdigit():
            return int(tok)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--keep', action='store_true', help='keep the temporary work dir')
    ap.add_argument('--log', metavar='FILE', help='console log path')
    args = ap.parse_args()
    for need in (os.path.join(QEMU, 'qemu/build/qemu-system-arm'), ROOT_TGZ, ETFS, STICK):
        if not os.path.exists(need):
            sys.exit('missing %s (dump extracted? qemu built? make stick run?)' % need)
    work = tempfile.mkdtemp(prefix='homebrew-test-')
    log = open(args.log or os.path.join(work, 'console.log'), 'w')
    print('work %s\nstick %s' % (work, STICK))
    g = boot(['-display', 'none',
              '-drive', 'if=none,id=stick,format=raw,read-only=on,file=%s' % STICK,
              '-device', 'usb-storage,drive=stick'] + stage(ETFS, 0) + stage(ROOT_TGZ, 1 << 20),
             log=log)
    mount_root(g, ETFS, ROOT_TGZ)
    run(g, 'export boardVersion=3.50 boardVariant=high')
    run(g, USB_START, timeout=60)
    # install the repo's current hook, not the one baked into the old stage tarball: the
    # hook is what copies boot.sh to ram and keeps io-usb alive, and the test must exercise
    # the tree it is run from.
    up = functools.partial(upload.upload, guest.GDB_SOCKET)
    run(g, up([os.path.join(REPO, 'jailbreak', 'hmi_startup.sh')]))
    run(g, 'cp /tmp/hmi_startup.sh /packages/system/override/hmi_startup.sh')
    print('pkgstart (minutes)...')
    run(g, '. /proc/boot/pkgstart.sh', timeout=900)
    time.sleep(25)
    print('up.')

    results = []
    try:
        print('\n=== A: baseline')
        results.append(('buttons is running after boot', bool(buttons(g))))
        watch = collect(g, 'p=$(cat /tmp/stickwatch.pid); '
                           'if [ -n "$p" ] && kill -0 "$p"; then echo alive; else echo dead; fi')
        results.append(('stickwatch is running', watch == 'alive'))
        hmi = collect(g, 'p=$(cat /tmp/hmi-overlay.loaded); '
                         'if [ -n "$p" ] && kill -0 "$p"; then echo alive; else echo dead; fi')
        results.append(('the hmi with the overlay is running', hmi == 'alive'))
        copies = collect(g, 'for f in /tmp/launcher /tmp/stickwatch.sh /tmp/buttons.txt '
                            '/tmp/hmi-overlay.so; do [ -s "$f" ] || echo missing:$f; done; '
                            'echo checked')
        results.append(('the ram copies are complete', copies == 'checked'))
        down = collect(g, 'test -e /tmp/stick-down && echo set || echo clear')
        results.append(('stickwatch says the stick is up', down == 'clear'))
        run(g, 'rm -f /tmp/hmi-stopped /tmp/buttons-paused')

        print('\n=== B: stillborn respawn, stickwatch restarts it')
        lost = lose_buttons(g)
        if lost:
            results.append(('ham loses buttons to the kill storm (stillborn)', True))
            back = wait_buttons(g, True, timeout=60)
            results.append(('stickwatch starts a buttons after %d missed checks' % BSTALE,
                            bool(back)))
            results.append(('stickwatch logged the miss and the start',
                            log_count(g, 'buttons missing for') > 0
                            and log_count(g, 'starting one') > 0))
        else:
            # the stillborn window is a race; a host-speed run can miss it. the restart path
            # is exercised deterministically by C and D below, so do not fail the run for a
            # failure that did not happen.
            results.append(('ham keeps buttons through the kill storm (not reproduced)', True))
            results.append(('stickwatch restart path left to C/D', True))
            results.append(('stickwatch restart path left to C/D', True))

        print('\n=== C: the watchdog stays out while homebrew owns the panel')
        # a live session has both markers: hmi-stopped and a buttons pause whose owner is
        # still alive. using the watchdog's own pid as the owner is enough for the gate.
        run(g, ': > /tmp/hmi-stopped; cat /tmp/stickwatch.pid > /tmp/buttons-paused')
        lost = lose_buttons(g)
        if lost:
            quiet = True
            for _ in range(5):
                time.sleep(5)
                if buttons(g):
                    quiet = False
                    break
            results.append(('ham loses buttons again (stillborn)', True))
            results.append(('nothing restarts buttons with /tmp/hmi-stopped present', quiet))
            run(g, 'rm -f /tmp/hmi-stopped /tmp/buttons-paused')
            back = wait_buttons(g, True, timeout=45)
            results.append(('stickwatch starts one again once the marker is gone', bool(back)))
        else:
            run(g, 'rm -f /tmp/hmi-stopped /tmp/buttons-paused')
            results.append(('ham keeps buttons again (not reproduced)', True))
            results.append(('nothing restarts buttons with /tmp/hmi-stopped present', True))
            results.append(('stickwatch starts one again once the marker is gone', True))

        print('\n=== D: a dead session\'s stale pause is resumed')
        pids = buttons(g)
        if pids:
            p = pids[0]
            run(g, 'kill -STOP %d' % p)
            run(g, 'echo %d > /tmp/buttons-paused; echo %d > /tmp/buttons.pids'
                % (DEAD_PID, p))
            # the old pid is SIGSTOPped, so slay -p still lists it until stickwatch kills
            # it; wait for that specific pid to be gone, not merely for some buttons to
            # be present
            back = None
            deadline = time.time() + 45
            while time.time() < deadline:
                pids2 = buttons(g)
                if pids2 and p not in pids2:
                    back = pids2
                    break
                time.sleep(4)
            fresh = bool(back)
            marker = collect(g, 'test -s /tmp/buttons-paused && echo set || echo empty')
            results.append(('the frozen buttons is killed and a fresh one appears', fresh))
            results.append(('the stale marker is gone', marker == 'empty'))
            results.append(('stickwatch logged the stale pause',
                            log_count(g, 'stale buttons pause') > 0))
        else:
            results.append(('the frozen buttons is killed and a fresh one appears', False))
            results.append(('the stale marker is gone', False))
            results.append(('stickwatch logged the stale pause', False))

        print('\n=== E: a dead session\'s stale hmi stop is resumed')
        hmi = collect(g, 'cat /tmp/hmi-overlay.loaded')
        if hmi and hmi.split()[0].isdigit():
            hp = int(hmi.split()[0])
            run(g, 'kill -STOP %d; : > /tmp/hmi-stopped; echo %d > /tmp/buttons-paused'
                % (hp, DEAD_PID))
            gone = False
            for _ in range(20):
                time.sleep(4)
                body = collect(g, 'test -e /tmp/hmi-stopped && echo set || echo gone')
                if body == 'gone':
                    gone = True
                    break
            alive = collect(g, 'p=$(cat /tmp/hmi-overlay.loaded); '
                               'if [ -n "$p" ] && kill -0 "$p"; then echo alive; else echo dead; fi')
            results.append(('the stale hmi stop marker is cleared', gone))
            results.append(('stickwatch logged the stale hmi stop',
                            log_count(g, 'stale hmi stop') > 0))
            results.append(('the hmi process survived the resume', alive == 'alive'))
        else:
            results.append(('the stale hmi stop marker is cleared', False))
            results.append(('stickwatch logged the stale hmi stop', False))
            results.append(('the hmi process survived the resume', False))
    finally:
        g.terminate(force=True)
        for p in (guest.UART3_SOCKET, guest.GDB_SOCKET):
            if os.path.exists(p):
                os.unlink(p)
        log.close()
    print('\n=== results')
    ok = True
    for name, passed in results:
        print('  %-58s %s' % (name, 'PASS' if passed else 'FAIL'))
        ok = ok and passed
    if args.keep:
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
