#!/bin/sh
# run the raw rgb565 player off the composite livi gadget's vendor interface. no port setup
# and no codec: the frames are already the panel's format. see rawplay/README.md; the host
# side is rawplay/out/rawlink.
#
# usage: rawplay.sh [seconds] [--stats] [--stage]
#   seconds  stop after this many seconds of guest time
#   --stage  always copy through the usbd_alloc staging ring instead of dma'ing straight
#            into the panel mapping (the default, which falls back on its own if refused)
#   --stats  five second progress lines
#
# the player sits next to this script: /tmp for an upload, /fs/usb0/... on the stick.
#
# the hmi is always stopped for the player (this script stops it when it is not stopped
# already, the launcher and the menu stop it around their commands), so the player is told
# to bring it back for the climate bar when a climate panel key is pressed. buttons is
# paused for the session, so the player restarts it for the window (kill + ham's clean
# respawn, the same way this script's resume does it) and stops it again at the end.
#
# whoever stops the hmi also stops the buttons service for the session: its events would
# otherwise queue in the stopped hmi and all replay at once when it resumes (volume jumps,
# menus open by themselves). buttons is frozen with SIGSTOP, not killed: it is guarded by
# ham (CONDDEATH + restart), so a kill is answered with an immediate respawn that would
# just queue again. at resume the frozen process is SIGKILLed; the guard starts a clean one
# and the kill is what drops the frames queued for the old connection, so the hmi sees the
# newest state only.
DIR=${0%/*}
[ "$DIR" = "$0" ] && DIR=/tmp

# background the old player: pdksh can miss the death of a child that exits at once, and a
# foreground slay here could wedge this script's shell before the player ever runs
/proc/boot/slay rawplay 2>/dev/null &
/proc/boot/sleep 1

HMI_PID=
BUTTONS_PAUSED=no
STOPPED=no

stop_hmi() {
    # buttons first: stopped before the hmi, it cannot be mid-write into a queue that will
    # only be read at resume. a marker whose owner is gone is stale (a session killed
    # before its resume), so take the pause over instead of trusting it.
    bp=$(cat /tmp/buttons-paused 2>/dev/null)
    if [ -n "$bp" ] && kill -0 "$bp" 2>/dev/null; then
        :   # a live outer session owns it
    else
        rm -f /tmp/buttons-paused
        # stop exactly the pids buttons has now: every later kill names pids, never the
        # name. a name-based slay scans /proc and can catch a fresh ham respawn before it
        # registers its CONDDEATH, after which nothing restarts it and the panel stays
        # deaf in the hmi (see common/hmictl.c).
        /proc/boot/slay -p buttons > /tmp/buttons.pids 2>/dev/null
        while read p; do
            [ -n "$p" ] && kill -STOP "$p" 2>/dev/null
        done < /tmp/buttons.pids
        echo $$ > /tmp/buttons-paused
        BUTTONS_PAUSED=yes
    fi
    HMI_PID=
    read HMI_PID < /tmp/hmi-overlay.loaded 2>/dev/null
    if [ -n "$HMI_PID" ]; then
        kill -STOP "$HMI_PID" 2>/dev/null
    else
        /proc/boot/slay -s SIGSTOP hmi
    fi
    STOPPED=yes
}

# a pid from slay -p only says a process exists: ham's respawn can die in the window before
# it registers its CONDDEATH and then no restart ever comes (found in the emulator, see
# common/hmictl.c). look, then look again after a settle. 1: buttons is there and stayed.
buttons_live() {
    /proc/boot/slay -p buttons > /tmp/buttons.pids 2>/dev/null
    [ -s /tmp/buttons.pids ] || return 1
    sleep 1
    /proc/boot/slay -p buttons > /tmp/buttons.pids 2>/dev/null
    [ -s /tmp/buttons.pids ]
}

resume_hmi() {
    if [ "$BUTTONS_PAUSED" = yes ] && [ -e /tmp/buttons-paused ]; then
        rm -f /tmp/buttons-paused
        # SIGKILL so the ham guard restarts it (see the header); it drops the backlog
        # before the hmi comes back, so nothing queued is delivered in the resume gap.
        # one listing, then only those pids: never a name-based slay that can catch the
        # fresh respawn before it has registered with ham.
        /proc/boot/slay -p buttons > /tmp/buttons.pids 2>/dev/null
        while read p; do
            [ -n "$p" ] && kill -9 "$p" 2>/dev/null
        done < /tmp/buttons.pids
        rm -f /tmp/buttons.pids
        # ham starts a clean one; if it did not (the killed process never registered, or its
        # respawn was stillborn the same way), start one here so the panel is not left deaf.
        # three rounds at most, each followed by buttons_live's settle: a pid is only proof
        # that a process exists, not that it registered with ham (see common/hmictl.c).
        attempt=0
        alive=no
        while [ "$alive" = no ] && [ $attempt -lt 3 ]; do
            attempt=$((attempt + 1))
            i=0
            while [ $i -lt 4 ] && [ "$alive" = no ]; do
                if buttons_live; then
                    alive=yes
                else
                    sleep 1
                    i=$((i + 1))
                fi
            done
            if [ "$alive" = no ]; then
                echo "rawplay.sh: buttons did not come back, starting one (attempt $attempt)"
                on -d /bin/buttons -G -+events >/dev/null 2>&1 &
                sleep 1
            fi
        done
        if [ "$alive" = no ] && buttons_live; then
            alive=yes      # the last start's confirmation
        fi
        rm -f /tmp/buttons.pids
        [ "$alive" = yes ] || echo "rawplay.sh: buttons is still missing, the panel is deaf"
    fi
    if [ -n "$HMI_PID" ]; then
        kill -CONT "$HMI_PID" 2>/dev/null
    else
        /proc/boot/slay -s SIGCONT hmi
    fi
    rm -f /tmp/hmi-stopped
}

if [ ! -e /tmp/hmi-stopped ]; then
    stop_hmi
    # only a run that stopped the hmi may resume it: inside the launcher/menu session the
    # outer command still owns the stop and the hmi must stay frozen behind it
    trap 'resume_hmi' EXIT INT TERM
    echo "rawplay.sh: hmi stopped, running player"
fi

$DIR/rawplay --usb --hmi "$@"
status=$?
# 75: the player found 64-byte bulk endpoints, i.e. the unit's io-usb is at full speed.
# restarting the stack without force_fs and trying once more is much better than playing
# 768 KB frames through a 12 mbit port at a frame or two a second.
if [ $status -eq 75 ]; then
    USBHS=$DIR/usbhs.sh
    [ -x "$USBHS" ] || USBHS=/fs/usb0/homebrew/apps/usbhs.sh
    if [ -x "$USBHS" ]; then
        echo "rawplay.sh: full-speed link, restarting the usb stack high speed"
        sh "$USBHS"
        $DIR/rawplay --usb --hmi "$@"
        status=$?
    fi
fi
echo "rawplay.sh: player exited $status"

if [ "$STOPPED" = yes ]; then
    resume_hmi
    # the player can have written the marker again while bringing the hmi back for a
    # climate key; the hmi is running now, so the next run must not think it is stopped
    rm -f /tmp/hmi-stopped
fi
echo "rawplay.sh: done"
