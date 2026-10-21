# jailbreak: the one file that goes on the unit's nand, at
# /packages/system/override/hmi_startup.sh. pkgstart sources it in place of the hmi package's
# own startup script, so it runs with the display and touch already up. it mounts the stick,
# hands over to the stick's boot.sh when it is there, and says on the hmi's own progress lines
# whether that worked. everything else (the launcher, its buttons, the apps) lives on the
# stick under /fs/usb0/homebrew/. see docs/homebrew.md.
BOOT=/fs/usb0/homebrew/boot.sh
BOOT_RAM=/tmp/homebrew-boot.sh
LOG=/fs/usb0/homebrew/jailbreak.log
TMPLOG=/tmp/hmi_startup.log
STICK_LOG=

# every step of the jailbreak goes to the stick's log so a unit with no console can be
# debugged by pulling the stick and reading it on a pc. until the stick is mounted lines go
# to /tmp; the first line that reaches a *writable* stick flushes what went to /tmp first, so
# the log still reads in order. the appliance serves the stick read-only, so testing for a
# directory and appending anyway only produced an iosetup error per line on the console and
# left /tmp carrying the whole log; -w is the mount-aware test qnx needs here.
log() {
    line="$(date +%H:%M:%S) hmi_startup[$$]: $*"
    if [ ! -e /tmp/stick-down ] && [ -w /fs/usb0/homebrew ]; then
        if [ -z "$STICK_LOG" ] && [ -s "$TMPLOG" ]; then
            # only clear /tmp once the lines are really on the stick
            cat "$TMPLOG" >> "$LOG" 2>/dev/null && : > "$TMPLOG"
        fi
        STICK_LOG=1
        echo "$line" >> "$LOG" 2>/dev/null && return
    fi
    echo "$line" >> "$TMPLOG" 2>/dev/null
}

log "start, args '$*' boardVariant=$boardVariant"

# mount the stick before looking at it: the stock media_player startup kills devb-umass (and
# its mount) before the hmi package starts, and the media restarts take io-usb down with it
# (its /dev entry can be left behind, so merely asking is not enough), so start both again the
# way startsys.sh and the test bench do. retry: those restarts can kill devb-umass mid-mount,
# and io-usb needs a moment to enumerate the stick after it is restarted. with no stick there
# is nothing to mount; the emulator's staged root bakes the stick's tree at /fs/usb0, so there
# boot.sh is found anyway
i=0
while [ $i -lt 3 ]
do
    log "mount attempt $((i + 1)): restarting devb-umass and io-usb_swsa"
    # first try with the io-usb that is already running. killing a healthy one wakes
    # aviage_monitor, which restarts the whole usb stack right as the hmi starts and drops
    # the mount we just made (the hmi then paints stock art and boot.sh's /tmp copies fail).
    # a later attempt still forces a restart if keeping it did not give a stable mount.
    slay -Q -f -s9 devb-umass 2>/dev/null
    if [ $i -gt 0 ] || [ -z "$(slay -p io-usb_swsa 2>/dev/null)" ]; then
        slay -Q -f -s9 io-usb_swsa 2>/dev/null
        sleep 1
        # no force_fs: that is what lets the usb83340 phy negotiate high speed (480 mbit),
        # which the raw uncompressed video needs. the stock startsys.sh line (and the media
        # player's restart.sh/usb_restart.sh) pass force_fs, so they pin the port at full
        # speed. see rawplay/README.md and usb/homebrew/apps/usbhs.sh
        io-usb_swsa -i20 -r3 -c -d ehci-mx31_swsa ioport=0x43f88100,irq=37,num_itd=300 &
        if waitfor /dev/io-usb/io-usb 10; then
            log "io-usb up"
        else
            log "io-usb did not come up in 10s"
        fi
        # the monitor can have decided to restart io-usb before it saw this one; mounting
        # on an instance that is about to be killed only throws the mount away. let that
        # restart land, then mount on whatever is still standing.
        sleep 2
        if [ -z "$(slay -p io-usb_swsa 2>/dev/null)" ]; then
            log "io-usb was restarted again under us, retrying"
            i=$((i + 1))
            continue
        fi
    else
        log "io-usb already running, keeping it"
    fi
    devb-umass cam pnp blk cache=2m,auto=partition,automount=hd0@dos:/fs/usb0,rw dos exe=all
    if waitfor /fs/usb0 8; then
        log "/fs/usb0 mounted"
    else
        log "/fs/usb0 did not mount in 8s"
    fi
    # the mount can be torn down after waitfor returns (the same media restart). only hand
    # over when boot.sh has been readable for three probes a second apart, so the hmi start
    # is not racing the next teardown.
    stable=0
    while [ $stable -lt 3 ]; do
        if read line < "$BOOT" 2>/dev/null; then
            stable=$((stable + 1))
        else
            stable=0
            break
        fi
        sleep 1
    done
    [ $stable -ge 3 ] && [ -x "$BOOT" ] && break
    log "no stable executable $BOOT yet (attempt $((i + 1)) of 3)"
    i=$((i + 1))
done

# show progress on the hmi, which hmiShow.sh draws nothing on until it finds the process: the
# hmi has only just been started (by boot.sh or the stock startup below), it throws away panel
# writes made before it initialises, so give it a moment, retry until it is found, then leave
# the message up for a moment and clear it
notify() {
    msg="$1"
    detail="$2"
    sleep 8
    i=0
    while [ $i -lt 20 ]
    do
        hmiPresent=
        . /proc/boot/hmiShow.sh "$msg"
        [ X"$hmiPresent" == Xyes ] && break
        sleep 1
        i=$((i + 1))
    done
    log "hmiShow '$msg': hmiPresent=$hmiPresent after $i tries"
    [ -n "$detail" ] && . /proc/boot/hmiShow.sh "$detail"
    sleep 5
    # hmiShow.sh reads "$#" for its no-argument clear path, and a sourced script keeps the
    # caller's positional parameters, so drop them before it gets a chance to redraw
    set --
    . /proc/boot/hmiShow.sh
}

if [ -x "$BOOT" ]; then
    # run boot.sh from ram, not from the stick. qnx's shell reads a script as it executes it,
    # and a mass-storage drop in the middle of that read hands it a truncated file (seen in
    # the emulator as "syntax error: if unmatched" at boot.sh[107]) even though the mount
    # probe passed a moment earlier. the copy is one small read while the mount is stable;
    # after that the script, and everything it starts, no longer depends on the stick.
    if cat "$BOOT" > "$BOOT_RAM" 2>/dev/null && chmod 755 "$BOOT_RAM" 2>/dev/null; then
        BOOT=$BOOT_RAM
        log "boot.sh copied to $BOOT_RAM"
    else
        log "could not copy boot.sh to ram, running it from the stick"
    fi
    echo "homebrew: jailbreak: running $BOOT"
    log "running $BOOT"
    . "$BOOT"
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "homebrew: jailbreak successful"
        log "boot.sh returned 0"
        notify "Jailbreak successful!"
    else
        echo "homebrew: jailbreak failed: boot.sh returned an error"
        log "boot.sh returned $rc"
        notify "Jailbreak failed." "boot.sh returned an error"
    fi
else
    if [ ! -d /fs/usb0 ]; then
        if [ -e /dev/hd0 ]; then
            why="usb stick found but it did not mount"
        else
            why="no usb stick present"
        fi
    elif [ ! -e "$BOOT" ]; then
        why="no homebrew/boot.sh on the stick"
    else
        why="boot.sh is not executable"
    fi
    echo "homebrew: jailbreak failed: $why"
    log "jailbreak failed: $why"
    . /scripts/hmi/startup.sh "$1"
    notify "Jailbreak failed." "$why"
fi

log "done"
