#!/bin/sh
# the stick side of the jailbreak. the only thing on the unit's nand is a hook that sources
# this file when it is present (see docs/homebrew.md), so everything here can be changed by
# pulling the stick and editing it on a pc.
#
# it starts the hmi with the overlay shim in its environment, swaps the hmi's touch connector
# for the launcher (which runs the commands in buttons.txt), so the hmi comes up with the
# carplay/apps plates. the homebrew menu is opened from the launcher's apps button; running it
# before the hmi is supported (the menu handles both modes) but not done here.
#
# it also runs by hand on a unit that is already up. a preload only applies at process start,
# so the overlay cannot reach a running hmi, and restarting it was not reliable (ham keeps
# resurrecting the stock hmi, so the two fight over the display). in that case nothing is
# drawn and nothing is started: hmiShow.sh shows the error and we quit, leaving the running
# hmi alone. reboot to get the buttons.
#
# every step goes to /fs/usb0/homebrew/jailbreak.log so a real unit can be debugged by pulling
# the stick; a stick that is not writable (the appliance always serves it read-only) falls
# back to /tmp/jailbreak.log. the hmi's own stdout/stderr go to the same log while it starts,
# so a loader error about the preload or a message from the shim is not lost to a console the
# unit does not have.
#
# the appliance's stock media stack can restart io-usb/devb-umass on its own schedule. that
# used to drop the mount while the hmi was starting, so the overlay missed every theme frame
# and the launcher/stickwatch copies were made from a dead mount (zero-byte launcher, no
# watchdog) - the car then had stock art, no footer buttons and no way to remount after a
# wake. the small pieces are now copied to /tmp and the watchdog is started *before* the hmi,
# and the mount is given a few seconds to settle first.

OVERLAY=/fs/usb0/hmi-overlay
SHIM=/fs/usb0/homebrew/hmi-overlay.so
STICK_LAUNCHER=/fs/usb0/homebrew/launcher
STICK_BUTTONS=/fs/usb0/homebrew/buttons.txt
STICK_WATCH=/fs/usb0/homebrew/stickwatch.sh
PROBE=/fs/usb0/homebrew/buttons.txt
STICK_LOG=/fs/usb0/homebrew/jailbreak.log
RAM_LOG=/tmp/jailbreak.log
LOG=$STICK_LOG
LAUNCHER=$STICK_LAUNCHER

# choose the log once, then follow the mount: on the appliance the stick is read-only, so
# writing to it would print a shell error on every line and still lose the line.
log() {
    line="$(date +%H:%M:%S) boot.sh[$$]: $*"
    if [ ! -e /tmp/stick-down ] && [ -w /fs/usb0/homebrew ]; then
        if [ "$LOG" != "$STICK_LOG" ] && [ -s "$RAM_LOG" ]; then
            cat "$RAM_LOG" >> "$STICK_LOG" 2>/dev/null && : > "$RAM_LOG"
        fi
        LOG=$STICK_LOG
    else
        LOG=$RAM_LOG
    fi
    echo "$line" >> "$LOG" 2>/dev/null
}

show_error() {
    echo "homebrew: $1"
    log "$1"
    . /proc/boot/hmiShow.sh "homebrew: $1"
    sleep 5
    . /proc/boot/hmiShow.sh
}

# copy a file into ram without leaving a zero-length replacement behind when the stick is
# gone halfway through. the old cat > target did, and /tmp/launcher then existed but could
# not run.
copy_ram() {
    [ -r "$1" ] || return 1
    cat "$1" > "$2.tmp" 2>/dev/null || {
        /bin/rm -f "$2.tmp" 2>/dev/null
        return 1
    }
    chmod 755 "$2.tmp" 2>/dev/null
    /bin/mv "$2.tmp" "$2" 2>/dev/null
}

probe() {
    read line < "$PROBE" 2>/dev/null
}

log "start, args '$*'"

if [ -e /hmi_/service ]; then
    # the hmi is already up. a preload only applies at process start, so the injected theme
    # elements cannot reach it, and restarting it was not reliable: ham keeps resurrecting
    # the stock hmi, so the two fight over the display (corrupted, no buttons). show the
    # error and leave the running hmi alone rather than breaking it.
    log "the hmi is already up (/hmi_/service is there), leaving it alone"
    show_error "restart the head unit to load the buttons"
    exit 1
fi

# get everything the hmi start and the session need into ram while the mount is still the
# one the hook just made. the shim is included: loading it off the stick at exec time is
# the other way the mount race used to take the overlay away.
SHIM_RAM=
copy_ram "$SHIM" /tmp/hmi-overlay.so && SHIM_RAM=/tmp/hmi-overlay.so
[ -n "$SHIM_RAM" ] && SHIM=$SHIM_RAM

copy_ram "$STICK_LAUNCHER" /tmp/launcher && LAUNCHER=/tmp/launcher
copy_ram "$STICK_BUTTONS" /tmp/buttons.txt
copy_ram "$STICK_WATCH" /tmp/stickwatch.sh

log "overlay $OVERLAY: $([ -d "$OVERLAY" ] && echo present || echo missing)"
log "shim $SHIM: $([ -r "$SHIM" ] && echo readable || echo missing)"

if [ -r "$SHIM" ]; then
    export LD_PRELOAD="$SHIM"
    log "LD_PRELOAD=$LD_PRELOAD"
else
    log "overlay disabled: no readable $SHIM"
fi

# start the watchdog before the hmi, so a media restart during startup or a wake later has
# someone to rebuild the mount. it probes every couple of seconds and keeps the old
# five-second interval's work, just sooner.
if [ -x /tmp/stickwatch.sh ]; then
    log "starting /tmp/stickwatch.sh"
    sh /tmp/stickwatch.sh >/dev/null 2>&1 &
else
    log "no stickwatch.sh on the stick"
fi

# the media stack can still restart io-usb just after the hook mounted. wait for three good
# probes (or eight seconds) before starting the hmi, so the overlay reads its frames from a
# mount that is not about to be torn down. the launcher and shim are already in ram, so a
# mount that never returns cannot stop the hmi from starting.
stable=0
waited=0
while [ $waited -lt 8 ]; do
    if probe; then
        stable=$((stable + 1))
    else
        stable=0
    fi
    [ $stable -ge 3 ] && break
    sleep 1
    waited=$((waited + 1))
done
log "stick stable for $stable probes after ${waited}s"
# tell the shim the stick root is safe to walk; stickwatch may have set its down marker
# between the probes above and its next pass
[ $stable -ge 3 ] && /bin/rm -f /tmp/stick-down 2>/dev/null

# the hmi writes its output here while it starts. the child keeps the fds after they are
# restored below, so the whole hmi run (and the stock touch connector and buttons it
# starts) logs to the same place.
HMI_LOG="$LOG"
[ -w "$HMI_LOG" ] || HMI_LOG=$RAM_LOG
log "starting the hmi (. /scripts/hmi/startup.sh), its output goes to $HMI_LOG"
exec 3>&1 4>&2
exec >> "$HMI_LOG" 2>&1
. /scripts/hmi/startup.sh "$1"
rc=$?
exec 1>&3 2>&4
exec 3>&- 4>&-
unset LD_PRELOAD
log "startup.sh returned $rc"

# the stock touch connector has to go before the launcher takes over touch, and only if the
# launcher is there: without it the hmi would be left with no touch at all. it is started
# asynchronously by the hmi startup, so kill it a few times over the next seconds; the
# launcher kills it once more just before it opens the device.
if [ -x "$LAUNCHER" ]; then
    log "stopping any old launcher and the stock touch connector"
    slay -f launcher 2>/dev/null
    slay -f touch_2_hmi_connector 2>/dev/null
    sleep 2
    slay -f touch_2_hmi_connector 2>/dev/null
    sleep 2
    slay -f touch_2_hmi_connector 2>/dev/null
    log "starting $LAUNCHER"
    "$LAUNCHER" &
    log "launcher pid $!"
else
    log "no launcher at $LAUNCHER, the stock touch connector stays"
fi

log "done"
true
