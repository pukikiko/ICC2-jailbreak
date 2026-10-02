#!/bin/sh
# run the media player off the stick. it opens its browser at /fs/usb0 (any argument is
# passed through: a file or directory, --lowres N, --loop, --stats).
#
# the launcher and the homebrew menu stop the hmi around every command and leave
# /tmp/hmi-stopped, so stopping it again is not needed there. run by hand, stop it with the
# kill builtin and the pid the overlay shim left: no child for the shell to wait on, which
# is what could hang a script that used slay for it (see rawplay/rawplay.sh).
#
# usage: mediaplayer.sh [/fs/usb0] [--lowres N] [--loop] [--stats]
DIR=${0%/*}
[ "$DIR" = "$0" ] && DIR=/tmp

STOPPED=no
if [ ! -e /tmp/hmi-stopped ]; then
    HMI_PID=
    read HMI_PID < /tmp/hmi-overlay.loaded 2>/dev/null
    if [ -n "$HMI_PID" ]; then
        kill -STOP "$HMI_PID" 2>/dev/null
        trap 'kill -CONT "$HMI_PID" 2>/dev/null' EXIT INT TERM
    else
        /proc/boot/slay -s SIGSTOP hmi
        trap '/proc/boot/slay -s SIGCONT hmi' EXIT INT TERM
    fi
    STOPPED=yes
    echo "mediaplayer.sh: hmi stopped, running player"
fi

"$DIR/mediaplayer" "$@"

if [ "$STOPPED" = yes ]; then
    if [ -n "$HMI_PID" ]; then
        kill -CONT "$HMI_PID" 2>/dev/null
    else
        /proc/boot/slay -s SIGCONT hmi
    fi
fi
echo "mediaplayer.sh: done"
