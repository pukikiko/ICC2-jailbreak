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
# the stick; a stick that is not writable (the emulator attaches it read-only) falls back to
# /tmp/jailbreak.log. the hmi's own stdout/stderr go to the same log while it starts, so a
# loader error about the preload or a message from the shim is not lost to a console the unit
# does not have.

OVERLAY=/fs/usb0/hmi-overlay
SHIM=/fs/usb0/homebrew/hmi-overlay.so
MARKER=/tmp/hmi-overlay.loaded
LAUNCHER=/fs/usb0/homebrew/launcher
LOG=/fs/usb0/homebrew/jailbreak.log

log() {
    line="$(date +%H:%M:%S) boot.sh[$$]: $*"
    echo "$line" >> "$LOG" 2>/dev/null || echo "$line" >> /tmp/jailbreak.log 2>/dev/null
}

show_error() {
    echo "homebrew: $1"
    log "$1"
    . /proc/boot/hmiShow.sh "homebrew: $1"
    sleep 5
    . /proc/boot/hmiShow.sh
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
else
    # the pre-hmi menu, if the stick has one. it returns to carry on into the hmi.
    #[ -x /fs/usb0/homebrew/main ] && /fs/usb0/homebrew/main

    log "overlay $OVERLAY: $([ -d "$OVERLAY" ] && echo present || echo missing)"
    log "shim $SHIM: $([ -r "$SHIM" ] && echo readable || echo missing)"
    if [ -d "$OVERLAY" ] && [ -r "$SHIM" ]; then
        export LD_PRELOAD="$SHIM"
        log "LD_PRELOAD=$LD_PRELOAD"
    else
        log "overlay disabled: no $OVERLAY directory or no readable $SHIM"
    fi

    # the hmi writes its output here while it starts. the child keeps the fds after they are
    # restored below, so the whole hmi run (and the stock touch connector and buttons it
    # starts) logs to the stick.
    HMI_LOG="$LOG"
    [ -w "$LOG" ] || HMI_LOG=/tmp/jailbreak.log
    log "starting the hmi (. /scripts/hmi/startup.sh), its output goes to $HMI_LOG"
    exec 3>&1 4>&2
    exec >> "$HMI_LOG" 2>&1
    . /scripts/hmi/startup.sh "$1"
    rc=$?
    exec 1>&3 2>&4
    exec 3>&- 4>&-
    unset LD_PRELOAD
    log "startup.sh returned $rc"
fi

# the launcher and its button map are small enough to keep in ram: a car lock/wake can drop
# the stick, and this keeps touch (and the hmi stop/resume) alive until stickwatch below has
# put the mount back. cat and chmod rather than cp/mkdir: /proc/boot has neither.
copy_ram() {
    [ -r "$1" ] || return
    cat "$1" > "$2" 2>/dev/null && chmod 755 "$2" 2>/dev/null
}
copy_ram "$LAUNCHER" /tmp/launcher
copy_ram /fs/usb0/homebrew/buttons.txt /tmp/buttons.txt
copy_ram /fs/usb0/homebrew/stickwatch.sh /tmp/stickwatch.sh
[ -x /tmp/launcher ] && LAUNCHER=/tmp/launcher

# the stock touch connector has to go before the launcher takes over touch, and only if the
# launcher is there: without it the hmi would be left with no touch at all. it is started
# asynchronously by the hmi startup, so kill it a few times over the next seconds; the
# launcher kills it once more just before it opens the device.
if [ -x "$LAUNCHER" ]; then
    log "stopping any old launcher and the stock touch connector"
    # a launcher from an earlier run would fight this one for the touch device
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

# started after the launcher so its first check finds one. it probes the homebrew stick and
# remounts it when a car wake unmounts it, watches the launcher pid, and flushes the log
# lines that went to /tmp while the stick was away.
if [ -x /tmp/stickwatch.sh ]; then
    log "starting /tmp/stickwatch.sh"
    sh /tmp/stickwatch.sh >/dev/null 2>&1 &
else
    log "no stickwatch.sh on the stick"
fi

log "done"
