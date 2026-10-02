#!/bin/sh
# synctool payload: hand over to the syncsploit ui, which installs the homebrew jailbreak
# hook on the unit's nand.
#
# runs as root, started by /etc/navi/synctool_check_and_exec.sh while the navi map update
# screen is up, before the factory synctool gets a chance to run. the cwd is
# /usr/navngo/synctool and stdout is the connector's, so log to files.
#
# first it lifts the connector's synctool key off the unit if it is still there, then execs
# the ui (`syncsploit/syncsploit.c`, carried on the stick beside this script): with the hmi frozen
# behind it the ui shows whether the unit is jailbroken, and its buttons either install
# jailbreak/hmi_startup.sh (embedded in the ui, byte for byte) to
# /packages/system/override/hmi_startup.sh - the slot pkgstart sources in place of the hmi
# package's own startup script - and reboot, or hand the screen straight back to the hmi.
# without the ui the scripted install below still works, for an older stick. the stick side
# of the jailbreak (homebrew/boot.sh, the launcher, the overlay, the apps) stays on the
# stick, so the stick has to be attached for the hook to find it. see docs/homebrew.md and
# syncsploit/README.md.

LOG=/tmp/synctool-payload.log
echo "synctool payload: $(id 2>/dev/null) cwd=$PWD" > "$LOG"

# the connector wrote this device's synctool key here just before the check script ran, and
# the script deletes it when it finishes. grab a copy while it is readable.
mount -uw /fs/usb0 2>/dev/null
if [ -r /dev/shmem/passwd ]; then
    cp /dev/shmem/passwd /fs/usb0/navi-synctool-key.bin 2>/dev/null
    echo "key copied: $(wc -c </dev/shmem/passwd) bytes" >> "$LOG"
fi

# the ui is the main path: it takes the screen, shows the jailbreak state, and installs the
# hook or cancels. exec it so killing the ui ends the payload with it. the candidates cover
# the stick's own tree (where this script and the ui sit together) and /tmp, where the
# emulator tests upload it. a stick built by mkexploit.py --ui carries it; without one the
# scripted install below is the fallback.
UI=
for cand in "$(dirname "$0")/syncsploit" /fs/usb0/synctool/syncsploit /tmp/syncsploit
do
    if [ -x "$cand" ]; then
        UI="$cand"
        break
    fi
done
if [ -n "$UI" ]; then
    echo "synctool payload: starting $UI" >> "$LOG"
    cp "$LOG" /fs/usb0/synctool-payload.log 2>/dev/null
    exec "$UI"
fi
echo "synctool payload: no syncsploit ui found, using the scripted install" >> "$LOG"

# show progress on the hmi, the way the boot scripts do: hmiShow.sh draws nothing until it
# finds the running hmi, so retry until it is up, then leave the message for a moment and
# clear it
notify() {
    msg="$1"
    detail="$2"
    i=0
    while [ $i -lt 20 ]
    do
        hmiPresent=
        . /proc/boot/hmiShow.sh "$msg"
        [ X"$hmiPresent" == Xyes ] && break
        sleep 1
        i=$((i + 1))
    done
    [ -n "$detail" ] && . /proc/boot/hmiShow.sh "$detail"
    sleep 5
    # hmiShow.sh reads "$#" for its no-argument clear path, and a sourced script keeps the
    # caller's positional parameters, so drop them before it gets a chance to redraw
    set --
    . /proc/boot/hmiShow.sh
}

# install the hook. /packages is the etfs nand and mounted read-write, and the override
# directory is the firmware's own slot, but make sure it is there anyway. the heredoc is
# quoted so the hook's own $variables stay literal
TARGET=/packages/system/override/hmi_startup.sh
mkdir -p /packages/system/override 2>/dev/null
cat > "$TARGET" <<'JAILBREAK'
# jailbreak: the one file that goes on the unit's nand, at
# /packages/system/override/hmi_startup.sh. pkgstart sources it in place of the hmi package's
# own startup script, so it runs with the display and touch already up. it mounts the stick,
# hands over to the stick's boot.sh when it is there, and says on the hmi's own progress lines
# whether that worked. everything else (the launcher, its buttons, the apps) lives on the
# stick under /fs/usb0/homebrew/. see docs/homebrew.md.
BOOT=/fs/usb0/homebrew/boot.sh
LOG=/fs/usb0/homebrew/jailbreak.log
TMPLOG=/tmp/hmi_startup.log
STICK_LOG=

# every step of the jailbreak goes to the stick's log so a unit with no console can be
# debugged by pulling the stick and reading it on a pc. until the stick is mounted lines go
# to /tmp; the first line that reaches the stick flushes what went to /tmp first, so the log
# still reads in order.
log() {
    line="$(date +%H:%M:%S) hmi_startup[$$]: $*"
    if [ -d /fs/usb0/homebrew ]; then
        if [ -z "$STICK_LOG" ] && [ -s "$TMPLOG" ]; then
            # only clear /tmp once the lines are really on the stick: with a read-only stick
            # (the emulator) the cat fails and the lines have to stay where they are
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
    slay -Q -f -s9 devb-umass 2>/dev/null
    slay -Q -f -s9 io-usb_swsa 2>/dev/null
    sleep 1
    # no force_fs: that is what lets the usb83340 phy negotiate high speed (480 mbit), which
    # the raw uncompressed video needs. the stock startsys.sh line (and the media player's
    # restart.sh/usb_restart.sh) pass force_fs, so they pin the port at full speed. see
    # rawplay/README.md and usb/homebrew/apps/usbhs.sh
    io-usb_swsa -i20 -r3 -c -d ehci-mx31_swsa ioport=0x43f88100,irq=37,num_itd=300 &
    if waitfor /dev/io-usb/io-usb 10; then
        log "io-usb up"
    else
        log "io-usb did not come up in 10s"
    fi
    devb-umass cam pnp blk cache=2m,auto=partition,automount=hd0@dos:/fs/usb0,rw dos exe=all
    if waitfor /fs/usb0 8; then
        log "/fs/usb0 mounted"
    else
        log "/fs/usb0 did not mount in 8s"
    fi
    [ -x "$BOOT" ] && break
    log "no executable $BOOT yet (attempt $((i + 1)) of 3)"
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
JAILBREAK

if [ $? -eq 0 ] && [ -s "$TARGET" ]; then
    chmod 755 "$TARGET" 2>/dev/null
    echo "jailbreak: wrote $TARGET ($(wc -c <"$TARGET") bytes)" >> "$LOG"
    notify "Jailbreak successful!" "hmi_startup.sh installed - restarting"
else
    echo "jailbreak: failed to write $TARGET" >> "$LOG"
    notify "Jailbreak failed." "could not write the override file"
fi

# keep this log on the stick too, next to the jailbreak log the hook writes on the way back up
cp "$LOG" /fs/usb0/synctool-payload.log 2>/dev/null

# restart so pkgstart sources the hook on the way back up (qnx_shutdown defaults to a reboot)
echo "jailbreak: rebooting" >> "$LOG"
shutdown
