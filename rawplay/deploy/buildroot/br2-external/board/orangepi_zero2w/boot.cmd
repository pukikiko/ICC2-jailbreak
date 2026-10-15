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
#
# The kernel is the uncompressed Image: measured on this board, the 38 MB
# read costs 1.65 s while Image.zst costs 0.5 s of read plus 2.6-3.1 s of
# U-Boot zstd decode - the uncompressed file is the faster of the two until
# the kernel itself is trimmed. An Image.zst is still bootable from this
# script (booti needs kernel_comp_addr_r and kernel_comp_size for it).
setenv bootargs root=/dev/mmcblk0p2 ro rootwait rootfstype=squashfs quiet loglevel=0 console=ttynull random.trust_cpu=on clk_ignore_unused pd_ignore_unused systemd.getty_auto=0
if load mmc 0:1 0x40080000 Image; then
    true
else
    setenv kernel_comp_addr_r 0x50000000
    load mmc 0:1 0x40080000 Image.zst
    setenv kernel_comp_size ${filesize}
fi
load mmc 0:1 0x4fa00000 sun50i-h618-orangepi-zero2w.dtb
booti 0x40080000 - 0x4fa00000
