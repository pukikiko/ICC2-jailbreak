# Release boot script. U-Boot's CONFIG_BOOTCOMMAND loads this from the boot
# FAT directly, so there is no distro-boot scan over USB, network or PXE.
#
# kernel cmdline:
#   root=/dev/mmcblk0p2 ro rootwait  the read-only squashfs root
#   quiet loglevel=0                 no kernel chatter on the car link
#   console=ttynull                  the release image has no serial console
#   random.trust_cpu=on              Chromium blocks on entropy at startup
#   clk_ignore_unused pd_ignore_unused  skip the late init that disables every
#                                   unused sunxi clock and power domain; the
#                                   appliance never needs the teardown
#   systemd.getty_auto=0             without this, console=ttynull makes
#                                   systemd's getty generator spawn a serial
#                                   getty on ttynull whose device unit never
#                                   appears (the udev coldplug is masked),
#                                   holding multi-user.target for 90 s
#
# T0 for this board is the power-apply; the serial log (debug image) or the
# first measured marker is what the README's table uses.
setenv bootargs root=/dev/mmcblk0p2 ro rootwait rootfstype=squashfs quiet loglevel=0 console=ttynull random.trust_cpu=on clk_ignore_unused pd_ignore_unused systemd.getty_auto=0
load mmc 0:1 0x40080000 Image
load mmc 0:1 0x4fa00000 sun50i-h618-orangepi-zero2w.dtb
booti 0x40080000 - 0x4fa00000
