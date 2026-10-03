#!/usr/bin/env python3
# Compute the squashfs offset inside a type-2 AppImage and unsquash it.
#
# An AppImage runtime is an ELF with the filesystem appended after the ELF
# image. The section header table is the last thing the runtime's linker
# data occupies, so the payload starts at:
#
#     max(e_shoff + e_shnum * e_shentsize, max(sh_offset + sh_size))
#
# This is the method the rawplay deployment notes describe (there is no
# unsquashfs -offset parser in older squashfs-tools, and the aarch64
# --appimage-extract runtime cannot execute on the x86 build host).
# We then look for the 'hsqs' magic at or after that offset and hand it to
# unsquashfs -o.
import argparse
import os
import struct
import subprocess
import sys


def elf_payload_offset(path):
    with open(path, "rb") as f:
        head = f.read(64)
        if head[:4] != b"\x7fELF":
            raise SystemExit("%s: not an ELF file" % path)
        if head[4] != 2:
            raise SystemExit("%s: not a 64-bit AppImage runtime" % path)
        little = head[5] == 1
        e = "<" if little else ">"
        e_shoff = struct.unpack_from(e + "Q", head, 0x28)[0]
        e_shentsize = struct.unpack_from(e + "H", head, 0x3A)[0]
        e_shnum = struct.unpack_from(e + "H", head, 0x3C)[0]
        table_end = e_shoff + e_shentsize * e_shnum
        max_end = 0
        for i in range(e_shnum):
            f.seek(e_shoff + i * e_shentsize)
            sh = f.read(e_shentsize)
            if len(sh) < e_shentsize:
                break
            _name, sh_type = struct.unpack_from(e + "II", sh, 0)
            sh_offset, sh_size = struct.unpack_from(e + "QQ", sh, 0x18)
            if sh_type != 8:  # SHT_NOBITS has no file content
                max_end = max(max_end, sh_offset + sh_size)
        return max(table_end, max_end)


def find_squashfs(path, start):
    with open(path, "rb") as f:
        f.seek(start)
        chunk = f.read(1 << 20)
        while chunk:
            i = chunk.find(b"hsqs")
            if i >= 0:
                return start + i
            start += len(chunk) - 3
            f.seek(start)
            chunk = f.read(1 << 20)
    raise SystemExit("%s: no squashfs superblock after the ELF" % path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--appimage", required=True)
    ap.add_argument("--unsquashfs", default="unsquashfs")
    ap.add_argument("--dest", required=True)
    args = ap.parse_args()

    payload = elf_payload_offset(args.appimage)
    offset = find_squashfs(args.appimage, payload)
    print("livi: ELF payload at 0x%x, squashfs at 0x%x (%d), extracting to %s"
          % (payload, offset, offset, args.dest))
    cmd = [args.unsquashfs, "-no-progress", "-o", str(offset), "-d", args.dest,
           args.appimage]
    subprocess.run(cmd, check=True)
    if not os.path.isfile(os.path.join(args.dest, "livi")):
        raise SystemExit("extraction completed but %s/livi is missing" % args.dest)


if __name__ == "__main__":
    sys.exit(main())
