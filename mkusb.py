#!/usr/bin/env python3
# builds usb.img, a partitioned fat32 stick image of usb/ for the emulator (and dd to a real
# stick). the unit wants an mbr with a fat partition, a bare fat gives it nothing
import os, shutil, struct, subprocess, sys
from PIL import Image  # noqa: F401 - mkhmiassets/mkmenuassets need it

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'hmi-overlay'))
sys.path.insert(0, os.path.join(HERE, 'hbmenu'))
import mkhmiassets
import mkmenuassets
SRC = os.path.join(HERE, 'usb')
OUT = os.path.join(HERE, 'usb.img')
SIZE_MB = 64
PART_TYPE = '0x0c'   # fat32 lba


START_LBA = 2048


def build_mtools(out):
    """the same mbr + fat32 image with mtools, for hosts without guestfish: build the fat
    partition on its own, then prepend the mbr and the partition's offset."""
    part = out + '.part'
    if os.path.exists(part):
        os.unlink(part)
    subprocess.run(['truncate', '-s', f'{SIZE_MB}M', part], check=True)
    subprocess.run(['mkfs.vfat', '-F', '32', '-n', 'HOMEBREW', part], check=True,
                   stdout=subprocess.DEVNULL)
    args = ['mcopy', '-s', '-i', part]
    args += sorted(os.path.join(SRC, name) for name in os.listdir(SRC))
    args += ['::']
    subprocess.run(args, check=True)
    sectors = os.path.getsize(part) // 512
    mbr = bytearray(512)
    off = 446
    mbr[off + 1:off + 4] = b'\xfe\xff\xff'
    mbr[off + 4] = int(PART_TYPE, 16)
    mbr[off + 5:off + 8] = b'\xfe\xff\xff'
    mbr[off + 8:off + 12] = struct.pack('<I', START_LBA)
    mbr[off + 12:off + 16] = struct.pack('<I', sectors)
    mbr[510:512] = b'\x55\xaa'
    with open(out, 'wb') as f:
        f.write(mbr)
        f.write(bytes((START_LBA - 1) * 512))
        with open(part, 'rb') as p:
            shutil.copyfileobj(p, f)
    os.unlink(part)


def main():
    # the theme frames are baked from the unit's own art in the ICC2 dump; without the
    # dump, keep whatever is already in usb/ (menu.raw is optional, the menu falls back)
    if os.path.isdir(os.path.join(os.environ.get('ICC2_DIR') or
                                  os.path.normpath(os.path.join(HERE, '..', '..', '..', 'ICC2')), 'dump')):
        mkhmiassets.build()
        mkmenuassets.build()
    else:
        print('mkusb: no firmware dump found, keeping existing usb/hmi-overlay and menu.raw')
    if os.path.exists(OUT):
        os.unlink(OUT)
    if shutil.which('guestfish'):
        subprocess.run(['truncate', '-s', f'{SIZE_MB}M', OUT], check=True)
        script = ['run', 'part-init /dev/sda mbr', 'part-add /dev/sda p 2048 -1',
                  f'part-set-mbr-id /dev/sda 1 {PART_TYPE}', 'mkfs vfat /dev/sda1', 'mount /dev/sda1 /']
        for name in sorted(os.listdir(SRC)):
            script.append(f'copy-in {os.path.join(SRC, name)} /')
        subprocess.run(['guestfish', '-a', OUT], input='\n'.join(script) + '\n', text=True, check=True)
    else:
        build_mtools(OUT)
    print('wrote', OUT)


if __name__ == '__main__':
    main()
