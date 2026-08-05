#!/bin/sh
#
# node.sh -- lifecycle for the Alpha lab node (rd vms-e2c).
#
# THE PROPERTY THIS EXISTS FOR: AXPbox terminates when its serial console client
# disconnects. Attaching with srmdrv.py, running one command and exiting takes
# the whole machine down with it. So a node that is meant to stay up runs a
# PERSISTENT console pump -- srmdrv.py in --fifo mode -- and commands are fed to
# that pump through the FIFO instead of by opening a new connection.
#
# This is the same shape as lab-1's nodedrv.py: one long-lived console per node,
# an input FIFO, and all console output tee'd to a log on the volume.
#
# Usage:
#   node.sh start           boot the machine and attach the console pump
#   node.sh send 'CMD'...   feed command lines to the running console
#   node.sh log [N]         tail the console log
#   node.sh status          is it up?
#   node.sh stop            stop the pump and the machine

set -eu

LAB="${LAB:-/data/training/vax/alpha}"
CFG="${CFG:-$LAB/cfg/alpha1.cfg}"
# AXPBOX selects the emulator build. Which release you run is a live variable
# here, not a settled one -- see README "Which AXPbox build".
AXPBOX="${AXPBOX:-$LAB/axpbox-1.2.0}"

# Releases before v1.2.0 link against SDL 1.2, which is not on this host and is
# NOT going to be installed on it (project rule: nothing on the host). setup.sh
# unpacks the .deb into $LAB/libs instead; point the loader at it if present.
if [ -d "$LAB/libs/usr/lib/x86_64-linux-gnu" ]; then
    LD_LIBRARY_PATH="$LAB/libs/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
    export LD_LIBRARY_PATH
fi
FIFO="$LAB/alpha1.fifo"
CONSOLE_LOG="$LAB/logs/console.log"
EMU_LOG="$LAB/logs/axpbox.log"
PORT=21264

emu_pid()  { pgrep -f "axpbox-.* run" || true; }
pump_pid() { pgrep -f "srmdrv.py" || true; }

# The console log carries the firmware's raw byte stream, which contains NULs.
# grep calls that a binary file and prints "binary file matches" instead of the
# line, so every grep over it needs -a.
cgrep() { grep -a "$@"; }

case "${1:-}" in
start)
    [ -n "$(emu_pid)" ] && { echo "already running (pid $(emu_pid))"; exit 0; }
    mkdir -p "$LAB/logs"
    rm -f "$FIFO"

    echo "starting AlphaServer ES40 with $CFG"
    cd "$LAB"
    nohup "$AXPBOX" run "$CFG" > "$EMU_LOG" 2>&1 < /dev/null &

    # Wait for the serial listener before attaching, otherwise the pump races
    # the emulator's startup and gets connection-refused.
    i=0
    while [ "$i" -lt 30 ]; do
        ss -ltn 2>/dev/null | grep -q ":$PORT " && break
        i=$((i + 1))
        sleep 1
    done
    ss -ltn 2>/dev/null | grep -q ":$PORT " || { echo "FAIL: no console listener on $PORT" >&2; exit 1; }

    echo "attaching persistent console pump"
    nohup python3 "$LAB/tools/srmdrv.py" -t 0 -f "$FIFO" -l "$CONSOLE_LOG" \
        > "$LAB/logs/pump.log" 2>&1 < /dev/null &

    # Do not claim success until the firmware actually reaches its prompt.
    i=0
    while [ "$i" -lt 60 ]; do
        cgrep -q 'P00>>>' "$CONSOLE_LOG" 2>/dev/null && break
        i=$((i + 1))
        sleep 1
    done
    if cgrep -q 'P00>>>' "$CONSOLE_LOG" 2>/dev/null; then
        cgrep -m1 'AlphaServer ES40 Console' "$CONSOLE_LOG" || true
        echo "up: SRM console ready (emulator $(emu_pid), pump $(pump_pid))"
    else
        echo "FAIL: never reached the P00>>> prompt; see $CONSOLE_LOG" >&2
        exit 1
    fi
    ;;

send)
    shift
    [ -p "$FIFO" ] || { echo "FAIL: no console FIFO at $FIFO -- is it started?" >&2; exit 1; }
    for cmd in "$@"; do
        printf '%s\n' "$cmd" > "$FIFO"
        sleep 2
    done
    tail -20 "$CONSOLE_LOG"
    ;;

log)
    tail -"${2:-40}" "$CONSOLE_LOG"
    ;;

status)
    e=$(emu_pid); p=$(pump_pid)
    [ -n "$e" ] && echo "emulator: running (pid $e)" || echo "emulator: STOPPED"
    [ -n "$p" ] && echo "console pump: running (pid $p)" || echo "console pump: STOPPED"
    [ -n "$e" ] && tail -3 "$CONSOLE_LOG" 2>/dev/null || true
    ;;

stop)
    p=$(pump_pid); [ -n "$p" ] && kill $p 2>/dev/null || true
    sleep 1
    e=$(emu_pid); [ -n "$e" ] && kill $e 2>/dev/null || true
    sleep 1
    rm -f "$FIFO"
    echo "stopped"
    ;;

*)
    sed -n '3,22p' "$0"
    exit 1
    ;;
esac
