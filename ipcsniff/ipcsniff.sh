#!/bin/sh
# start/stop the passive ipc sniffer without stopping the hmi or buttons.
#
# usage: ipcsniff.sh start|stop|status
#
# the sniffer reads /dev/ipc/0, ipc's monitor tap. start detaches it with `on -d`, so
# the menu or terminal that launched it can exit and the hmi/buttons keep running. the
# log is replaced on every start: /fs/usb0/homebrew/ipc.log when the stick is writable,
# /tmp/ipc.log otherwise.
DIR=${0%/*}
[ "$DIR" = "$0" ] && DIR=/tmp
SNIFF=$DIR/ipcsniff
[ -x "$SNIFF" ] || SNIFF=/fs/usb0/homebrew/apps/ipcsniff
PIDFILE=/tmp/ipc-sniff.pid
LOG=/fs/usb0/homebrew/ipc.log
[ -w /fs/usb0/homebrew ] || LOG=/tmp/ipc.log

case "$1" in
start)
    if [ -r "$PIDFILE" ]; then
        read pid < "$PIDFILE" 2>/dev/null
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            echo "ipcsniff already running pid $pid, log $LOG"
            exit 0
        fi
    fi
    rm -f "$PIDFILE"
    : > "$LOG" 2>/dev/null || LOG=/tmp/ipc.log
    : > "$LOG" 2>/dev/null
    if [ -x /proc/boot/on ]; then
        /proc/boot/on -d "$SNIFF" --log "$LOG" >/dev/null 2>&1 &
    else
        "$SNIFF" --log "$LOG" >/dev/null 2>&1 &
    fi
    i=0
    while [ $i -lt 10 ]; do
        if [ -r "$PIDFILE" ]; then
            read pid < "$PIDFILE" 2>/dev/null
            if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
                echo "ipcsniff running pid $pid, log $LOG"
                exit 0
            fi
        fi
        sleep 1
        i=$((i + 1))
    done
    echo "ipcsniff failed to start, log $LOG"
    exit 1
    ;;
stop)
    if [ -r "$PIDFILE" ]; then
        read pid < "$PIDFILE" 2>/dev/null
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null
            i=0
            while [ $i -lt 5 ] && kill -0 "$pid" 2>/dev/null; do
                sleep 1
                i=$((i + 1))
            done
            kill -9 "$pid" 2>/dev/null
        fi
        rm -f "$PIDFILE"
        echo "ipcsniff stopped"
        exit 0
    fi
    pids=`/proc/boot/slay -p ipcsniff 2>/dev/null`
    if [ -n "$pids" ]; then
        kill $pids 2>/dev/null
        echo "ipcsniff stopped"
        exit 0
    fi
    echo "ipcsniff not running"
    exit 1
    ;;
status)
    if [ -r "$PIDFILE" ]; then
        read pid < "$PIDFILE" 2>/dev/null
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            echo "ipcsniff running pid $pid, log $LOG"
            exit 0
        fi
    fi
    echo "ipcsniff not running, log $LOG"
    exit 1
    ;;
*)
    echo "usage: $0 start|stop|status"
    exit 2
    ;;
esac
