#!/usr/bin/env python3
"""pull the 128 byte secrets out of a firmware image or a raw binary.

the navi synctool checksum file (`<name>.enc`) is AES-256-CBC over the md5 of `<name>`, and
the password is a 128 byte blob that navi_2_hmi_connector writes to /dev/shmem/passwd. the
blob is a build time constant: it is never derived from the serial, the version, the map part
number or anything else the unit shows, and nothing on the unit ever copies it to removable
media. the same pattern (a fixed 128 byte buffer written out) is used by `ipc -M` and `ipc -D`
for the package update passwords. this finds those writes instead of hardcoding addresses, so
it works for any build where the compiler emitted the same shape:

    ldr r1, [pc, #imm]     ; the buffer
    mov r2, #0x80          ; 128 bytes
    bl  write

    ./findkeys.py --elf dump/packages/factory/navi/root/bin/navi_2_hmi_connector
    ./findkeys.py --tree dump --write extracted/
"""
import argparse
import hashlib
import os
import struct
import sys

PASSWD = b'/dev/shmem/passwd'


class Elf:
    def __init__(self, data):
        self.d = data
        if data[:4] != b'\x7fELF':
            raise ValueError('not an elf')
        self.little = data[5] == 1
        if not self.little or data[4] != 1:
            raise ValueError('only little endian elf32')
        phoff, = struct.unpack_from('<I', data, 28)
        phnum, = struct.unpack_from('<H', data, 44)
        self.segs = []
        for i in range(phnum):
            t, off, va, pa, fs, ms, fl, al = struct.unpack_from('<8I', data, phoff + i * 32)
            if t == 1:
                self.segs.append((va, off, fs, fl))

    def read(self, va, n):
        for va0, off, fs, fl in self.segs:
            if va0 <= va and va + n <= va0 + fs:
                return self.d[off + va - va0: off + va - va0 + n]
        return None

    def u32(self, va):
        b = self.read(va, 4)
        return struct.unpack('<I', b)[0] if b else None

    def readable(self, va, n):
        return self.read(va, n) is not None


def scan_128_byte_writes(e):
    """find ldr r1,[pc,#imm] + mov r2,#0x80 + bl, any alignment/segment."""
    hits = []
    seen = set()
    for va0, off, fs, fl in e.segs:
        if not fl & 1:                                  # not executable
            continue
        for i in range(0, fs - 4, 4):
            w = struct.unpack_from('<I', e.d, off + i)[0]
            a = va0 + i
            if w not in (0xe3a02080, 0xe3b02080):       # mov/movs r2, #0x80
                continue
            for j in range(1, 8):
                if i - 4 * j < 0:
                    break
                p = struct.unpack_from('<I', e.d, off + i - 4 * j)[0]
                pa = a - 4 * j
                if p & 0xfffff000 == 0xe59f1000:                 # ldr r1, [pc, #imm]
                    target = pa + 8 + (p & 0xfff)
                elif p & 0xfffff000 == 0xe51f1000:               # ldr r1, [pc, #-imm]
                    target = pa + 8 - (p & 0xfff)
                else:
                    continue
                need_bl = any(
                    struct.unpack_from('<I', e.d, off + i + 4 * k)[0] & 0xff000000 == 0xeb000000
                    for k in range(1, 6) if i + 4 * k + 4 <= fs)
                if not need_bl:
                    continue
                buf = e.u32(target)
                if buf is None or buf in seen:
                    continue
                if not any(v0 <= buf and buf + 128 <= v0 + fs and fl & 2
                           for v0, _o, fs, fl in e.segs):
                    continue                                # constant pools, not a writable key
                blob = e.read(buf, 128)
                if blob is None:
                    continue
                seen.add(buf)
                hits.append((a, target, buf, blob))
                break
    return hits


def openssl_password(blob):
    """what openssl 0.9.8 -pass file: would use out of this blob"""
    line = blob[:1024].split(b'\n', 1)[0][:1023]
    if line.endswith(b'\r'):
        line = line[:-1]
    nul = line.find(b'\0')
    return line[:nul] if nul >= 0 else line


def report(name, path, blob, index, outdir, is_synctool=False, buf=None):
    pw = openssl_password(blob)
    kind = 'synctool-key' if is_synctool else 'secret-%d' % index
    print('%s  %s  buffer=%s' % (name, path, ('%#x' % buf) if buf is not None else '?'))
    print('    blob md5   %s' % hashlib.md5(blob).hexdigest())
    print('    password   %s  (%d bytes, openssl -pass file: semantics)' % (pw.hex(), len(pw)))
    print('    hex        %s' % blob.hex())
    if outdir:
        fn = os.path.join(outdir, ('%s.bin' % kind))
        if os.path.exists(fn):
            base, ext = os.path.splitext(fn)
            fn = '%s-%d%s' % (base, index, ext)
        open(fn, 'wb').write(blob)
        print('    wrote      %s' % fn)


def do_elf(path, outdir):
    with open(path, 'rb') as f:
        if f.read(4) != b'\x7fELF':
            return 0
        f.seek(0)
        data = f.read()
    try:
        e = Elf(data)
    except ValueError:
        return 0
    hits = scan_128_byte_writes(e)
    synctool = PASSWD in data
    for i, (code, pool, buf, blob) in enumerate(hits):
        report(os.path.basename(path), path, blob, i, outdir, synctool, buf)
    return len(hits)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--elf', metavar='FILE', help='one binary (navi_2_hmi_connector, ipc, ...)')
    ap.add_argument('--tree', metavar='DIR', help='a dump tree, scans every regular file')
    ap.add_argument('--write', metavar='DIR', help='also write each blob out as a file')
    ap.add_argument('--addr', metavar='BUFHEX[,LEN]',
                    help='manual: read a buffer at a vaddr instead of scanning')
    args = ap.parse_args()
    if args.write:
        os.makedirs(args.write, exist_ok=True)
    if args.addr:
        if not args.elf:
            sys.exit('--addr needs --elf')
        e = Elf(open(args.elf, 'rb').read())
        buf, _, length = args.addr.partition(',')
        blob = e.read(int(buf, 0), int(length or 128, 0))
        if blob is None:
            sys.exit('cannot read %s at that address' % args.addr)
        report('manual', args.elf, blob, 0, args.write)
        return
    total = 0
    if args.elf:
        total += do_elf(args.elf, args.write)
    if args.tree:
        for root, dirs, files in os.walk(args.tree):
            for fn in sorted(files):
                p = os.path.join(root, fn)
                total += do_elf(p, args.write)
    if not total:
        sys.exit('no 128 byte write buffers found')


if __name__ == '__main__':
    main()
