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
# read by hmi-overlay: while this exists, do not touch the stick root for theme assets.
# a dead devb-umass read can sit in a 30 s timeout; skipping it keeps the hmi responsive
# while stickwatch rebuilds the mount. missing means "try the stick" (old sticks/startup).
DOWNMARK=/tmp/stick-down
# the pid of the io-usb instance the hook/usbhs started without force_fs. if the stock
# aviage monitor replaces it (its own restart line has force_fs), the pid changes even when
# the mount comes back before the next probe; compare and put high speed back.
HSPIDFILE=/tmp/io-usb.hs
RAMLOG=/tmp/jailbreak.log
SELFLOG=/tmp/stickwatch.log
PIDFILE=/tmp/stickwatch.pid
INTERVAL=2
STALE=2
# the unit's /tmp is a ram disk. a car can run for days with the stick read-only, and
# hmi-overlay logs every theme asset while the hmi starts and redraws; keep the three
# files that can grow without a writer-side cap bounded here, so a runaway log can never
# take the ram disk (and with it the hmi) down. jlog caps itself too; this covers the
# hmi's own stdout/stderr redirected to the same file by boot.sh.
CAP_KB=1024

log() {
    echo "$(date +%H:%M:%S) stickwatch[$$]: $*" >> "$SELFLOG" 2>/dev/null
}

cap_log() {
    [ -f "$1" ] || return 0
    size=
    for w in $(du -k "$1" 2>/dev/null); do
        size=$w
        break
    done
    if [ -n "$size" ] && [ "$size" -gt "$CAP_KB" ]; then
        : > "$1" 2>/dev/null
        log "truncated $1 at ${size}k"
    fi
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
    # the old driver has to be gone before the new one opens the partition; a SIGKILL
    # returns from slay at once but the process can still be unwinding its device
    sleep 1
    # a mass-storage LUN that went through a usb reset can stay wedged even though
    # /dev/io-usb/io-usb still exists; restarting only devb-umass then loops forever on a
    # dead LUN. after one failed remount, restart the controller too, which re-enumerates
    # the gadget and gives the guest mass-storage function a fresh start.
    if [ "$force_usb" = yes ] || [ ! -e /dev/io-usb/io-usb ]; then
        log "restarting io-usb (high speed) to reset the mass-storage link"
        slay -Q -f -s9 io-usb_swsa 2>/dev/null
        sleep 1
        io-usb_swsa -i20 -r3 -c -d ehci-mx31_swsa ioport=0x43f88100,irq=37,num_itd=300 &
        waitfor /dev/io-usb/io-usb 10
        force_usb=no
    fi
    devb-umass cam pnp blk cache=2m,auto=partition,automount=hd0@dos:/fs/usb0,rw dos exe=all
    waitfor "$STICK" 8
    if probe; then
        log "stick is back"
        hs_pid=$(echo $(slay -p io-usb_swsa 2>/dev/null))
        echo "$hs_pid" > "$HSPIDFILE" 2>/dev/null
        if [ -s "$RAMLOG" ]; then
            cat "$RAMLOG" >> "$STICK/homebrew/jailbreak.log" 2>/dev/null && : > "$RAMLOG"
        fi
        return 0
    fi
    force_usb=yes
    return 1
}

launcher_alive() {
    # no pid file means no new launcher has run (or /tmp was wiped, which takes this script
    # with it): treat it as alive rather than kill and restart something that may be there
    [ -r /tmp/launcher.pid ] || return 0
    lpid=$(cat /tmp/launcher.pid 2>/dev/null)
    [ -n "$lpid" ] && kill -0 "$lpid" 2>/dev/null
}

# ---------------------------------------------------------------- buttons watchdog ---
# ham guards the stock buttons service (CONDDEATH + restart) and normally has a clean one
# running within milliseconds of a kill. a respawn that dies in the window before it
# registers its CONDDEATH raises nothing, no restart ever follows, and the panel is left
# with no buttons for good (found in the emulator by hammering the session kill: ham
# printed "Restarted", the new pid was gone a second later and never came back; see
# common/hmictl.c). start one when that has happened - but only when nobody else can be
# handling it: a homebrew session with the hmi stopped owns buttons through the pause
# marker, and ham's respawn must have had time ($BSTALE checks) before it counts missing.
PAUSEMARK=/tmp/buttons-paused
BPIDS=/tmp/buttons.pids
HMIMARK=/tmp/hmi-overlay.loaded
HMISTOP=/tmp/hmi-stopped
BSTALE=3

hmi_is_up() {
    # only the overlay marker names the hmi for sure; without it nothing here can tell
    # whether the pid is a hmi, so leave buttons alone (boot.sh's hmiShow path covers the
    # no-shim case)
    [ -e "$HMISTOP" ] && return 1
    hp=$(cat "$HMIMARK" 2>/dev/null)
    [ -n "$hp" ] && kill -0 "$hp" 2>/dev/null
}

resume_stale_pause() {
    # a session that died before its resume leaves the pause marker with its owner gone and
    # the buttons it stopped still frozen: the panel would stay deaf until the next session.
    # homebrew is not running (its owner is dead) and hmi_is_up() already held.
    po=$(cat "$PAUSEMARK" 2>/dev/null)
    [ -n "$po" ] || return 0
    kill -0 "$po" 2>/dev/null && return 0
    log "stale buttons pause (owner $po gone), resuming buttons"
    /bin/rm -f "$PAUSEMARK" 2>/dev/null
    # kill only recorded pids that are still buttons: one whose pid was reused after the
    # death of the frozen process must not be taken down with it
    if [ -s "$BPIDS" ]; then
        cur=" $(echo $(slay -p buttons 2>/dev/null)) "
        for p in $(cat "$BPIDS" 2>/dev/null); do
            case "$cur" in
            *" $p "*) kill -9 "$p" 2>/dev/null ;;
            esac
        done
    fi
}

resume_stale_hmi() {
    # a homebrew session that is killed (a crash, `slay`, the guard's SIGKILL) never runs its
    # resume, so the hmi stays SIGSTOPped behind /tmp/hmi-stopped and every later session
    # sees the marker and believes the panel is already stopped: the screen is frozen until
    # a reboot. only act when no live session owns the buttons pause; a live launcher/menu
    # command is deliberately holding the hmi and must not be resumed underneath it.
    [ -e "$HMISTOP" ] || return 0
    po=$(cat "$PAUSEMARK" 2>/dev/null)
    if [ -n "$po" ] && kill -0 "$po" 2>/dev/null; then
        return 0
    fi
    log "stale hmi stop (owner ${po:-none} gone), resuming the hmi"
    resume_stale_pause
    hp=$(cat "$HMIMARK" 2>/dev/null)
    if [ -n "$hp" ] && kill -0 "$hp" 2>/dev/null; then
        kill -CONT "$hp" 2>/dev/null
    fi
    /bin/rm -f "$HMISTOP" 2>/dev/null
}

buttons_watch() {
    # caller checked hmi_is_up
    resume_stale_pause
    if [ -n "$(slay -p buttons 2>/dev/null)" ]; then
        bmisses=0
    else
        bmisses=$((bmisses + 1))
        if [ $bmisses -ge $BSTALE ]; then
            log "buttons missing for $bmisses checks, starting one"
            on -d /bin/buttons -G -+events >/dev/null 2>&1 &
            bmisses=0
        fi
    fi
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
bmisses=0
caploop=0
force_usb=no
hs_pid=$(cat "$HSPIDFILE" 2>/dev/null)
while :; do
    cur_usb=$(echo $(slay -p io-usb_swsa 2>/dev/null))
    if [ -n "$hs_pid" ] && [ "$cur_usb" != "$hs_pid" ]; then
        log "io-usb was replaced ($hs_pid -> ${cur_usb:-none}), forcing high speed"
        force_usb=yes
        if remount; then
            /bin/rm -f "$DOWNMARK" 2>/dev/null
        fi
        hs_pid=$(echo $(slay -p io-usb_swsa 2>/dev/null))
        echo "$hs_pid" > "$HSPIDFILE" 2>/dev/null
        continue
    fi
    if probe; then
        misses=0
        /bin/rm -f "$DOWNMARK" 2>/dev/null
    else
        # the first miss is enough to tell the shim not to walk into the dead mount; a
        # single transient failure only costs a frame's worth of stock art, not a 30 s stall
        : > "$DOWNMARK" 2>/dev/null
        misses=$((misses + 1))
        if [ $misses -ge $STALE ]; then
            if remount; then
                misses=0
                /bin/rm -f "$DOWNMARK" 2>/dev/null
            else
                log "remount failed, will keep trying"
                sleep 20
                continue
            fi
        fi
    fi
    resume_stale_hmi
    if ! launcher_alive; then
        restart_launcher
    fi
    if hmi_is_up; then
        buttons_watch
    else
        bmisses=0
    fi
    # the log caps do not need a du process every probe; ten loops is 20 s, still far less
    # than an unbounded writer needs to hurt the ram disk
    caploop=$((caploop + 1))
    if [ $caploop -ge 10 ]; then
        caploop=0
        cap_log "$RAMLOG"
        cap_log /tmp/hmi_startup.log
        [ -w "$STICK/homebrew" ] && cap_log "$STICK/homebrew/jailbreak.log"
        cap_log "$SELFLOG"
    fi
    sleep $INTERVAL
done
