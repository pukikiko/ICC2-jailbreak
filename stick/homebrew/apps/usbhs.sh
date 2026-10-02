#!/bin/sh
# put the unit's usb port into high speed (480 mbit).
#
# the usb83340 phy on the board is a hi-speed part, but the stock startsys.sh starts
# io-usb_swsa with force_fs, which pins the port at full speed (12 mbit, about 1 MB/s of
# bulk). the raw uncompressed path needs the high speed link.
#
# io-usb_swsa without force_fs lets the phy negotiate high speed. the jailbreak hook already
# restarts it that way (see docs/homebrew.md and rawplay/README.md), but the stock media
# player's restart.sh/usb_restart.sh put force_fs back when they restart io-usb, and a
# running unit may be sitting at full speed because of one. this script restarts the stack
# the high speed way again; run it before starting the player and check the player's first
# line (it prints "high speed" or "full speed" for the vendor bulk endpoints).
#
# run it from ram when you can: restarting io-usb unmounts /fs/usb0, which is where this
# script lives if you ran it from the stick. it re-execs itself from /tmp in that case.
#
#     upload ../usb/homebrew/apps/usbhs.sh
#     sh /tmp/usbhs.sh
#
case "$0" in
    /fs/usb0/*)
        cp "$0" /tmp/usbhs.sh 2>/dev/null && exec /bin/sh /tmp/usbhs.sh "$@"
        ;;
esac

echo "usbhs: restarting the usb stack without force_fs"
slay -Q -f -s9 devb-umass 2>/dev/null
slay -Q -f -s9 io-usb_swsa 2>/dev/null
sleep 1
# the high speed line: no force_fs. the media player's restarts add it back, so this is the
# line to re-run if the link falls back
io-usb_swsa -i20 -r3 -c -d ehci-mx31_swsa ioport=0x43f88100,irq=37,num_itd=300 &
waitfor /dev/io-usb/io-usb 10
devb-umass cam pnp blk cache=2m,auto=partition,automount=hd0@dos:/fs/usb0,rw dos exe=all
waitfor /fs/usb0 8
echo "usbhs: io-usb_swsa up without force_fs"
if [ -x /proc/boot/usb ]; then
    /proc/boot/usb 2>/dev/null | grep -i "speed" | head -4
fi
echo "usbhs: now start the player; its first line says high speed or full speed"
