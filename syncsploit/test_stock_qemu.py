#!/usr/bin/env python3
"""synctool on a stock unit, in the emulator, with nothing but a usb stick.

test_qemu.py boots a unit with the jailbreak hook already staged and runs the stock check
script by hand over the can console. this test is the harder question: does the same stick
work against an *unmodified* unit, one with no hook and no can/uart access to set anything
up? it boots the dump with navi in the ram root and no override file
(`qemu/mkstage.py` NAVI_RAM=1 NO_HOOK=1), lets the stock package startup bring navi and its
`navi_2_hmi_connector` up, hotplugs a partitioned fat32 stick the way a person plugs one in,
and brings the usb port up the way the homebrew hook does. the stock
`/etc/navi/synctool_check_and_exec.sh` then runs the stick's own `payload` (the real
`syncsploit/payload.sh`), as root, and the payload installs the hook and lifts the connector's
key onto the stick.

one honest gap: the factory *trigger*. the map update screen belongs to igo
(`sumitomo_sw-qnxarm-release`), which dies in the emulated graphics stack before it can ask
the connector for an update (ham restarts it in a loop). so the harness presses the button igo
would press by running the script the connector would run, with the connector's environment
(the `startsys.sh` PATH with its empty first field, and the key the connector writes to
`/dev/shmem/passwd`). everything from that line on is the unit's own stock code.

the run is observed without shelling into the guest: hmi screenshots while the payload reports
on the hmi's progress lines, and the stick image read back on the host afterwards for the
lifted key. run it with `python3 syncsploit/test_stock_qemu.py [--keep]`.
"""
import argparse
import functools
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
ICC2 = os.environ.get('ICC2_DIR') or os.path.normpath(os.path.join(REPO, '..', '..', '..', 'ICC2'))
QEMU = os.path.join(ICC2, 'qemu')
DUMP = os.path.join(ICC2, 'dump')
NAVI = os.path.join(DUMP, 'packages/factory/navi/root/bin/navi_2_hmi_connector')
ETFS = os.path.join(DUMP, 'packages/factory/qnx_binaries_and_libraries/root/sbin/fs-etfs-ram')
STAGE = os.path.join(QEMU, 'stage-navi-ram-stock.tar.gz')
PAYLOAD = os.path.join(HERE, 'payload.sh')
HOOK = os.path.join(REPO, 'jailbreak', 'hmi_startup.sh')
UI = os.path.join(REPO, 'out', 'syncsploit')
QEMU_BIN = os.path.join(QEMU, 'qemu/build/qemu-system-arm')

sys.path.insert(0, QEMU)
import guest, qmp, upload, v850
from guest import GDB_SOCKET, UART3_SOCKET, boot, mount_root, run, stage

# the environment startsys.sh gives the packages, empty first field and all (line 22)
UNIT_PATH = ':/packages/system/override:/bin:/usr/bin:/usr/sbin:/sbin:/proc/boot'
# the usb port startsys.sh brings up before packages do (line 147..153), the stand-in for the
# unit's own usb driver coming up at boot. the media_player startup kills the stack and then
# blocks on the missing audio device before it gets to mcd, so on a real unit mcd would mount
# the stick here and in the emulator these lines do
USB_START = [
    'slay -Q -f -s9 devb-umass; slay -Q -f -s9 io-usb_swsa; sleep 1',
    'io-usb_swsa -i20 -r3 -c -d ehci-mx31_swsa ioport=0x43f88100,irq=37,num_itd=300 &'
    ' waitfor /dev/io-usb/io-usb 10',
]
# the car's eeprom value navi's startup waits for before it will start igo and the connector
NAVI_SERIAL = 280394325


class Qmp(qmp.Qmp):
    def shot(self, path):
        self.cmd('screendump', filename=path)

    def click(self, x, y, hold=0.3):
        pos = [{'type': 'abs', 'data': {'axis': a, 'value': int(v * 32767 / (size - 1))}}
               for a, v, size in (('x', x, 800), ('y', y, 480))]
        self.cmd('input-send-event', events=pos)
        for down in (True, False):
            self.cmd('input-send-event',
                     events=[{'type': 'btn', 'data': {'button': 'left', 'down': down}}])
            time.sleep(hold)


def build_stage():
    if os.path.exists(STAGE):
        return
    env = dict(os.environ, NAVI_RAM='1', NO_HOOK='1')
    subprocess.run([sys.executable, os.path.join(QEMU, 'mkstage.py')], check=True, env=env)


def build_stick(work):
    """a fat32 stick with /synctool, built exactly the way a real one is (mkexploit.py)"""
    tree = os.path.join(work, 'stick', 'synctool')
    img = os.path.join(work, 'stick.img')
    subprocess.run([sys.executable, os.path.join(HERE, 'mkexploit.py'),
                    '--out', tree, '--payload', PAYLOAD, '--name', 'payload', '--ui', UI,
                    '--image', img], check=True, capture_output=True)
    subprocess.run([sys.executable, os.path.join(HERE, 'findkeys.py'),
                    '--elf', NAVI, '--write', work], check=True, capture_output=True)
    found = [os.path.join(work, f) for f in os.listdir(work) if 'synctool-key' in f]
    if not found:
        sys.exit('findkeys.py did not find the synctool key in %s' % NAVI)
    keyfile = os.path.join(work, 'navi-key.bin')
    shutil.move(found[0], keyfile)
    return img, keyfile


def fat_file(img, short):
    """read a file out of the fat16 image on the host by its 8.3 name.

    mtools chokes on the long-name entries the unit's dos driver leaves when it is killed
    mid-flush, so the root directory is walked here instead: partition at 1MB, fat16 only."""
    data = open(img, 'rb').read()
    part = 1 << 20
    bs = data[part:part + 64]
    bps, spc = struct.unpack_from('<H', bs, 11)[0], bs[13]
    res, nfats = struct.unpack_from('<H', bs, 14)[0], bs[16]
    nroot = struct.unpack_from('<H', bs, 17)[0]
    fatsz = struct.unpack_from('<H', bs, 22)[0]
    fat = part + res * bps
    root = fat + nfats * fatsz * bps
    data_start = root + nroot * 32
    for i in range(nroot):
        e = data[root + i * 32:root + i * 32 + 32]
        if not e or e[0] == 0:
            break
        if e[0] == 0xe5 or (e[11] & 0x0f) == 0x0f:
            continue
        if e[11] & 0x10:
            continue
        if e[:len(short)].upper() != short.upper().encode():
            continue
        size = struct.unpack_from('<I', e, 28)[0]
        cluster = struct.unpack_from('<H', e, 26)[0]
        out = b''
        while cluster >= 2 and len(out) < size:
            off = data_start + (cluster - 2) * spc * bps
            out += data[off:off + spc * bps]
            cluster = struct.unpack_from('<H', data, fat + cluster * 2)[0]
        return out[:size]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--keep', action='store_true', help='keep the temporary work dir')
    args = ap.parse_args()
    for need in (QEMU_BIN, ETFS, NAVI, PAYLOAD):
        if not os.path.exists(need):
            sys.exit('missing %s (dump extracted? qemu built?)' % need)
    build_stage()
    work = tempfile.mkdtemp(prefix='synctool-stock-')
    img, keyfile = build_stick(work)
    key = open(keyfile, 'rb').read()
    key_md5 = hashlib.md5(key).hexdigest()
    print('work  %s\nstick %s (%d MB)\nkey   %d bytes, md5 %s'
          % (work, img, os.path.getsize(img) >> 20, len(key), key_md5))
    log = open(os.path.join(work, 'console.log'), 'w')
    # the stick is hotplugged, so it starts life as a plain drive; cache=writethrough makes the
    # payload's writes land in the image file while qemu runs, for the host side check
    g = boot(['-display', 'none', '-qmp', f'unix:{guest.QMP_SOCKET},server=on,wait=off',
              '-drive', f'if=none,id=stick,format=raw,cache=writethrough,file={img}']
             + stage(ETFS, 0) + stage(STAGE, 1 << 20), log=log)
    micro = v850.V850(v850.connect(UART3_SOCKET), lambda m: None)
    micro.uploader = functools.partial(upload.upload, GDB_SOCKET)
    q = Qmp(guest.QMP_SOCKET)
    results = []
    try:
        print('booted, unpacking the stock root (this is the emulator standing in for the nand)')
        mount_root(g, ETFS, STAGE)
        run(g, 'ln -sP / /fs/etfs')            # startsys.sh:133, the nand is /fs/etfs too
        run(g, 'export boardVersion=3.50 boardVariant=high')
        print('the car hands over its navi serial')
        v850.command(micro, 'set vehicle.config navi_serial %d' % NAVI_SERIAL)
        print('stock package startup (navi included, no hook staged)')
        run(g, '. /proc/boot/pkgstart.sh', timeout=900)
        v850.command(micro, 'set vehicle.config navi_serial %d' % NAVI_SERIAL)
        # the media_player startup restarts the usb stack a few times before it settles, and
        # the stick has to go in after that; wait it out the way a person would
        time.sleep(120)
        # pidin truncates the name, so match the tail
        ps = run(g, 'pidin | grep _connector').replace('\r', '')
        print('navi_2_hmi_connector:', ps.strip())
        results.append(('stock navi connector running',
                        any('_connector' in l and 'RECEIVE' in l for l in ps.splitlines())))

        print('plugging the stick in (qmp hotplug, the way a person inserts one)')
        print('device_add ->', q.cmd('device_add', driver='usb-storage', id='stick0', drive='stick'))
        time.sleep(3)
        # on a real unit the media change daemon mounts the stick when it appears. here the
        # media_player startup blocks on the missing audio device before it ever starts mcd,
        # so the test brings the port up itself with the same lines the homebrew hook uses
        print('the unit brings the usb port up and mounts the stick')
        mounted = ''
        for attempt in range(3):
            for line in USB_START:
                run(g, line, timeout=60)
            run(g, 'devb-umass cam pnp blk cache=2m,auto=partition,automount=hd0@dos:/fs/usb0,rw'
                   ' dos exe=all', timeout=60)
            time.sleep(5)
            mounted = run(g, 'ls /fs/usb0/synctool 2>&1; ls /dev/hd* 2>&1').replace('\r', '')
            if 'payload' in mounted:
                break
            print('  no mount yet, retrying')
        print('stick:', mounted.strip())
        results.append(('stock devb-umass mounted the stick', 'payload' in mounted))

        # the connector writes the key to /dev/shmem/passwd before the check script runs; play
        # that part so the payload can lift it, then run the script from the connector's
        # environment (this is the harness standing in for igo's map update request)
        v850.command(micro, 'upload ' + keyfile)
        time.sleep(3)
        run(g, 'cat /tmp/navi-key.bin > /dev/shmem/passwd')
        print('running the stock check script with the unit PATH')
        g.send('PATH=%s /bin/sh /etc/navi/synctool_check_and_exec.sh; echo SCRIPT_DONE\r'
               % UNIT_PATH)
        # the payload lifts the key, then execs the syncsploit ui. wait for the ui to come up,
        # screenshot it, and press its Jailbreak button (syncsploit/syncsploit.c: 224x62 at x 160..384,
        # y 338..400)
        print('waiting for the syncsploit ui')
        payload_log = ''
        hook_md5 = ''
        for i in range(4):
            time.sleep(3)
            q.shot(os.path.join(work, 'ui-%02d.ppm' % i))
        print('pressing Jailbreak')
        q.click(272, 369)
        time.sleep(2)
        q.shot(os.path.join(work, 'ui-jailbreak.ppm'))
        # the ui leaves "installed - restarting" up for five seconds before the reboot, so the
        # installed hook and the payload's log can be read back while the unit is still there
        for i in range(6):
            try:
                hook_md5 = run(g, 'md5sum /packages/system/override/hmi_startup.sh',
                               timeout=10).replace('\r', '')
                payload_log = run(g, 'cat /tmp/synctool-payload.log',
                                  timeout=10).replace('\r', '')
            except Exception:
                hook_md5 = ''
                payload_log = '(guest is gone)'
            if hook_md5.strip() and payload_log.strip():
                break
            time.sleep(2)
        for i in range(3):
            time.sleep(3)
            q.shot(os.path.join(work, 'ui-restart-%02d.ppm' % i))
        print('payload log:', payload_log.strip() or '(empty)')
        print('hook on the unit:', hook_md5.strip() or '(not read)')
        results.append(('payload ran as root and wrote its log', 'synctool payload' in payload_log))
        results.append(('payload started the syncsploit ui',
                        'starting' in payload_log and 'syncsploit' in payload_log))
        results.append(('ui installed the hook byte for byte',
                        hashlib.md5(open(HOOK, 'rb').read()).hexdigest() in hook_md5.lower()))
        # let the guest's dos driver flush the stick before qemu is killed, or the directory
        # the payload wrote is left half updated
        try:
            run(g, 'umount /fs/usb0 2>/dev/null; echo UMOUNTED', timeout=20)
        except Exception:
            pass
    finally:
        g.terminate(force=True)
        for p in (guest.QMP_SOCKET, UART3_SOCKET, guest.SER5_SOCKET, GDB_SOCKET):
            if os.path.exists(p):
                os.unlink(p)
        log.close()

    # host side: what the payload and the ui left on the stick
    lifted = fat_file(img, 'NAVI-S~1BIN')
    stick_log = fat_file(img, 'SYNCTO~1LOG') or b''
    results.append(('payload copied the key onto the stick', lifted is not None))
    results.append(('lifted key matches the firmware key',
                    lifted is not None and hashlib.md5(lifted).hexdigest() == key_md5))
    results.append(('ui wrote the install to the stick log', b'jailbreak installed' in stick_log))
    print('\n=== results')
    ok = True
    for name, passed in results:
        print('  %-46s %s' % (name, 'PASS' if passed else 'FAIL'))
        ok = ok and passed
    print('\nhmi screenshots in', work)
    if not args.keep:
        print('(use --keep to keep the work dir)')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
