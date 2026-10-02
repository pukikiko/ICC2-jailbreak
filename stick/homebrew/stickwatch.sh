#!/bin/sh
# stickwatch - keep the homebrew stick mounted across a car sleep/wake.
#
# the stock media stack tears the mount down when the car sleeps and on usb restarts: the
# aviage monitor restarts io-usb_swsa (with force_fs) and slays devb-umass, and nothing but
# the boot hook ever mounts /fs/usb0 again. the hmi keeps painting the overlaid footer (it
# already has the assets) but the launcher's button commands and buttons.txt are on the
# stick, so the plates go dead until the next boot. this waits for the stick to go and puts
# it back the way jailbreak/hmi_startup.sh does, at high speed (no force_fs).
#
# boot.sh copies this to /tmp and starts it from there, because the stick it watches is the
# stick it would otherwise be running from. it also checks the launcher's pid file (written
# by launcher/launcher.c) and starts it again on the stick if a wake killed it. every step goes
# to /tmp/stickwatch.log; /tmp/jailbreak.log, which jlog falls back to while the stick is
# away, is flushed into the stick's log once the mount is back.
#
# the unit has no mkdir/cp/pidin, so this uses the shell builtins plus the /proc/boot tools
# the boot scripts already use (cat, slay, waitfor, sleep, date, chmod).

STICK=/fs/usb0
PROBE="$STICK/homebrew/buttons.txt"
RAMLOG=/tmp/jailbreak.log
SELFLOG=/tmp/stickwatch.log
PIDFILE=/tmp/stickwatch.pid
INTERVAL=5
STALE=3

log() {
    echo "$(date +%H:%M:%S) stickwatch[$$]: $*" >> "$SELFLOG" 2>/dev/null
}

if [ -r "$PIDFILE" ]; then
    old=$(cat "$PIDFILE" 2>/dev/null)
    if [ -n "$old" ] && kill -0 "$old" 2>/dev/null; then
        log "already running as $old"
        exit 0
    fi
fi
echo $$ > "$PIDFILE"

# one read: missing mount fails with ENOENT, a mount whose device went away fails with EIO
probe() {
    read line < "$PROBE" 2>/dev/null
}

remount() {
    log "stick not readable, remounting"
    slay -Q -f -s9 devb-umass 2>/dev/null
    if [ ! -e /dev/io-usb/io-usb ]; then
        log "io-usb is gone, restarting it (high speed)"
        slay -Q -f -s9 io-usb_swsa 2>/dev/null
        sleep 1
        io-usb_swsa -i20 -r3 -c -d ehci-mx31_swsa ioport=0x43f88100,irq=37,num_itd=300 &
        waitfor /dev/io-usb/io-usb 10
    fi
    devb-umass cam pnp blk cache=2m,auto=partition,automount=hd0@dos:/fs/usb0,rw dos exe=all
    waitfor "$STICK" 8
    if probe; then
        log "stick is back"
        if [ -s "$RAMLOG" ]; then
            cat "$RAMLOG" >> "$STICK/homebrew/jailbreak.log" 2>/dev/null && : > "$RAMLOG"
        fi
        return 0
    fi
    return 1
}

launcher_alive() {
    # no pid file means no new launcher has run (or /tmp was wiped, which takes this script
    # with it): treat it as alive rather than kill and restart something that may be there
    [ -r /tmp/launcher.pid ] || return 0
    lpid=$(cat /tmp/launcher.pid 2>/dev/null)
    [ -n "$lpid" ] && kill -0 "$lpid" 2>/dev/null
}

restart_launcher() {
    log "launcher is not running, restarting it"
    slay -f launcher 2>/dev/null
    slay -f touch_2_hmi_connector 2>/dev/null
    sleep 1
    if [ -x /tmp/launcher ]; then
        /tmp/launcher >/dev/null 2>&1 &
    elif [ -x "$STICK/homebrew/launcher" ]; then
        "$STICK/homebrew/launcher" >/dev/null 2>&1 &
    fi
}

log "start, probing $PROBE every ${INTERVAL}s"
misses=0
while :; do
    if probe; then
        misses=0
    else
        misses=$((misses + 1))
        if [ $misses -ge $STALE ]; then
            if remount; then
                misses=0
            else
                log "remount failed, will keep trying"
                sleep 20
                continue
            fi
        fi
    fi
    if ! launcher_alive; then
        restart_launcher
    fi
    sleep $INTERVAL
done
