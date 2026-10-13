#!/bin/sh
# restart - restart the whole ICC through the unit's own shutdown.
#
# the stock /proc/boot/shutdown is the intended way down: stop ham so nothing resurrects,
# slay the dumper and the usb mass storage, then exec qnx_shutdown, which terminates the
# processes, unmounts the filesystems and resets the soc - qnx_shutdown ends in
# sysmgr_reboot, so the unit comes back on its own and boots straight into the jailbreak
# (the hook on the nand mounts the stick and boot.sh starts the hmi with the overlay). it
# is the same reset a car package update finishes with and the one the syncsploit ui uses.
#
# the one thing a script has to get right is not being waited on: qnx_shutdown stops dead
# at "Shutting down filesystems..." when the process that spawned it is still waiting for
# it (syncsploit/README.md), and the launcher and the homebrew menu wait for every command
# they run. the shutdown script is therefore started detached (`on -d`) and this script
# returns at once, so the session hands the panel back and the reset runs unattended.
#
#     usb/homebrew/apps/restart.sh      the menu row
#     sh /tmp/restart.sh                by hand (upload it to /tmp first)

log() {
    line="$(date +%H:%M:%S) restart[$$]: $*"
    if [ -d /fs/usb0/homebrew ]; then
        echo "$line" >> /fs/usb0/homebrew/jailbreak.log 2>/dev/null && return
    fi
    echo "$line" >> /tmp/jailbreak.log 2>/dev/null
}

log "restarting the unit (stock shutdown, detached)"

# /bin/sh runs the stock script (it has no shebang); its exec qnx_shutdown replaces that
# shell, and `on -d` leaves it with no parent waiting on it
on -d /bin/sh /proc/boot/shutdown >/dev/null 2>&1

log "shutdown started; the unit comes back on its own"
