# Release boot script. U-Boot's CONFIG_BOOTCOMMAND loads this from the boot
# FAT directly, so there is no distro-boot scan over USB, network or PXE.
#
# kernel cmdline:
#   root=/dev/mmcblk0p2 ro rootwait  the read-only squashfs root
#   quiet loglevel=0                 no kernel chatter on the car link
#   console=ttynull                  the release image has no serial console
#   random.trust_cpu=on              Chromium blocks on entropy at startup
#
# T0 for this board is the power-apply; the serial log (debug image) or the
# first measured marker is what the README's table uses.
setenv bootargs root=/dev/mmcblk0p2 ro rootwait rootfstype=squashfs quiet loglevel=0 console=ttynull random.trust_cpu=on
load mmc 0:1 0x40080000 Image
load mmc 0:1 0x4fa00000 sun50i-h618-orangepi-zero2w.dtb
booti 0x40080000 - 0x4fa00000
