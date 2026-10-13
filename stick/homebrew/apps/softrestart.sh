#!/bin/sh
# softrestart - restart the unit's packages without rebooting.
#
# the full restart (apps/restart.sh) goes through the stock shutdown: ham stops, the usb
# mass storage drops, qnx_shutdown unmounts everything and resets the soc. this one leaves
# qnx, its drivers and the driver packages (graphics, io, qnx/swsa binaries) running and
# only takes the application packages down and starts them again - power, bluetooth,
# media_player, hmi, applications, diagnostics and navi when it is installed - in that
# order, with the same environ and startup scripts pkgstart sources. ham goes down first
# (`hamctrl -stop`) so the kills are not answered with immediate respawns, and comes back
# before the packages so their CONDDEATH registrations land on the fresh instance. the
# display and touch never stop, so this costs seconds where the full restart costs a boot.
#
# media_player's startup is the stock one, so it slays devb-umass the way it always does
# and /fs/usb0 goes with it. the stick is mounted again the hook's way (no force_fs, so
# the port keeps high speed) when it has not come back by itself; stickwatch.sh, when it
# is running, is trying to do the same and whichever of the two gets there first wins.
#
# the hmi comes back with the overlay shim when the stick carries one, and a running
# homebrew launcher stays the touch connector: the stock touch connector the hmi startup
# starts is slain, exactly like boot.sh does it. the hmi's old pid marker is replaced by
# the shim of the new hmi, so the launcher's stop/resume keeps working.
#
# a homebrew session owns the panel while a menu command runs, and the menu and launcher
# restore the frozen hmi frame and resume the hmi when the command returns. the restart
# would fight that (and the old frame would be painted over the fresh hmi), so when a
# session is up this script returns at once and a detached worker does the work, first
# waiting for the session to hand the panel back.
#
#     usb/homebrew/apps/softrestart.sh      the menu row
#     sh /tmp/softrestart.sh                by hand (upload it to /tmp first)

LOGSTICK=/fs/usb0/homebrew/jailbreak.log
LOGTMP=/tmp/jailbreak.log

log() {
    line="$(date +%H:%M:%S) softrestart[$$]: $*"
    if [ -d /fs/usb0/homebrew ]; then
        echo "$line" >> "$LOGSTICK" 2>/dev/null && return
    fi
    echo "$line" >> "$LOGTMP" 2>/dev/null
}

# run from ram: media_player's startup drops devb-umass and /fs/usb0 with it, and a shell
# reading the script off the stick would die mid-run when that happens (usbhs.sh re-execs
# itself for the same reason)
case "$0" in
    /fs/usb0/*)
        cat "$0" > /tmp/softrestart.sh 2>/dev/null &&
            exec /bin/sh /tmp/softrestart.sh "$@"
        ;;
esac

# a homebrew session is up: hand the restart to a detached worker and return so the menu
# or launcher can restore its frame and resume the hmi with a live process behind it. the
# worker waits for /tmp/hmi-stopped (the session marker, unlinked on the resume) and only
# then takes the packages down.
if [ "$1" != "--worker" ] && [ -e /tmp/hmi-stopped ]; then
    log "a homebrew session owns the panel, restarting from a detached worker"
    on -d /bin/sh /tmp/softrestart.sh --worker >/dev/null 2>&1
    exit 0
fi

# one at a time: a stale pid file (the worker died) is taken over
if [ -r /tmp/softrestart.pid ]; then
    old=$(cat /tmp/softrestart.pid 2>/dev/null)
    if [ -n "$old" ] && kill -0 "$old" 2>/dev/null; then
        log "already running as $old"
        exit 0
    fi
fi
echo $$ > /tmp/softrestart.pid

if [ "$1" = "--worker" ]; then
    log "worker up, waiting for the session to hand the panel back"
    i=0
    while [ -e /tmp/hmi-stopped ] && [ $i -lt 20 ]; do
        sleep 1
        i=$((i + 1))
    done
    log "session over after ${i}s, restarting"
fi

# nothing is done about the APS wrapper variables here (DefaultPartition, apsHmi,
# apsMedia, apsNavi): they were set by the package environ files when pkgstart ran and are
# inherited from the session, and on this build (useAPS=small, startsys.sh) they are empty
# on purpose - the packages just run plain. the environ files are not re-sourced either:
# their `aps create` fails against the partitions made at boot and takes them down with it
# (found the hard way), and there is nothing to create at this point anyway.

log "stopping ham"
hamctrl -stop 2>/dev/null

# the package processes. media_player's startup kills its own stack again below; this is
# the explicit take-down, and it is also what stops them while no ham exists to respawn
# them. driver packages are deliberately not in the list: i2c/spi/io-audio/io-display and
# the rest stay running.
log "stopping the package processes"
slay -f pmm 2>/dev/null
slay -f bc5 bcsp bluetoothdiagnostics bccmd bluetoothdfu bluetoothprofiles 2>/dev/null
slay -f aviage_monitor media-player io-media-generic mme-generic io-fs-media mcd qdb \
    enum-devices enum-usb 2>/dev/null
slay -f hmi touch_2_hmi_connector buttons 2>/dev/null
slay -f time_manager acm him vehicle_settings 2>/dev/null
slay -f diagnostics 2>/dev/null
slay -f gps igo_starter navi_2_hmi_connector igo 2>/dev/null
sleep 1

# ham first: the packages attach to it as they come up
log "starting ham"
$DefaultPartition ham ${ham_param:--V1} &

# one package's startup, the way pkgstart sources it: the override slot wins over /scripts
start_package() {
    pkg="$1"

    log "starting $pkg"
    if [ -e "/packages/system/override/${pkg}_startup.sh" ]; then
        . "/packages/system/override/${pkg}_startup.sh"
    elif [ -e "/scripts/${pkg}/startup.sh" ]; then
        . "/scripts/${pkg}/startup.sh"
    fi
}

start_package power
start_package bluetooth
start_package media_player

# media_player restarted the usb mass storage; give mcd and stickwatch a moment to put the
# stick back, then do the hook's remount when it is missing or read-only (the enum-started
# devb-umass is a read-only one). the hmi needs the stick for its overlay, so this has to
# happen before it starts.
waitfor /fs/usb0 8
if [ ! -w /fs/usb0 ]; then
    log "the stick is not mounted read-write, remounting it the hook's way"
    slay -Q -f -s9 devb-umass 2>/dev/null
    if [ ! -e /dev/io-usb/io-usb ]; then
        log "io-usb is gone, restarting it (high speed)"
        slay -Q -f -s9 io-usb_swsa 2>/dev/null
        sleep 1
        io-usb_swsa -i20 -r3 -c -d ehci-mx31_swsa ioport=0x43f88100,irq=37,num_itd=300 &
        waitfor /dev/io-usb/io-usb 10
    fi
    devb-umass cam pnp blk cache=2m,auto=partition,automount=hd0@dos:/fs/usb0,rw dos exe=all
    waitfor /fs/usb0 8
    log "stick: $([ -w /fs/usb0 ] && echo back || echo still missing)"
fi

# the hmi, with the overlay shim in its environment when the stick has one - boot.sh's
# half of the chain, minus the launcher (it is already running). the override slot is
# deliberately not used here: on a jailbroken unit it is the hook itself, and sourcing it
# would mount the stick and hand over to boot.sh all over again (notify sleeps and all),
# not restart the package. the shim has to be readable: LD_PRELOAD with a path that is
# not there is a loader error for the hmi (boot.sh makes the same check).
log "starting hmi (stock startup)"
if [ -d /fs/usb0/hmi-overlay ] && [ -r /fs/usb0/homebrew/hmi-overlay.so ]; then
    export LD_PRELOAD=/fs/usb0/homebrew/hmi-overlay.so
    log "hmi with LD_PRELOAD=$LD_PRELOAD"
fi
[ -e /scripts/hmi/startup.sh ] && . /scripts/hmi/startup.sh
unset LD_PRELOAD

# keep the homebrew launcher as the touch connector: it already owns the touch device and
# reopens /hmi_/service on use. the stock connector the hmi startup just started would
# fight it for the device, so slay it (boot.sh kills it the same way). without a live
# launcher the stock connector is the only touch, and it stays.
lpid=$(cat /tmp/launcher.pid 2>/dev/null)
if [ -n "$lpid" ] && kill -0 "$lpid" 2>/dev/null; then
    log "launcher $lpid is live, slaying the stock touch connector"
    slay -f touch_2_hmi_connector 2>/dev/null
    sleep 2
    slay -f touch_2_hmi_connector 2>/dev/null
else
    log "no live launcher, the stock touch connector stays"
fi

start_package applications
start_package diagnostics
start_package navi

log "done"
