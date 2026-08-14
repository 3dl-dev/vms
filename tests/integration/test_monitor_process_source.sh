#!/bin/sh
#
# test_monitor_process_source.sh - BEHAVIOURAL + STRUCTURAL gate (vms-c840):
# MONITOR PROCESS and SHOW SYSTEM list the SAME set of processes because they
# read the SAME single source -- the executive process table via
# vms_kif_procscan() -- and MONITOR keeps no independent /proc scan of its own.
#
# WHY THIS EXISTS.
# Operator-observed 2026-08-14: `MONITOR PROCESS` and `SHOW SYSTEM` showed
# DIFFERENT process lists. Root cause: tools/vms_monitor.c built its list by
# walking the /proc directory and each per-PID stat file under it (every Linux
# task), while src/vmsdcl/dcl_cmd_show.c's cmd_show_system() reads the
# executive's process table through vms_kif_procscan(). Two different sources
# answered the same VMS question with two different lists. The fix unifies the
# source: MONITOR now reads vms_kif_procscan() too, so the two commands are
# consistent BY CONSTRUCTION -- whatever the executive's table holds.
#
# SCOPE (operator 2026-08-14). This gate asserts CONSISTENCY, not a policy on
# WHAT the executive table contains. Whether the table includes any given
# class of task is the executive's call and is under separate review; this gate
# only requires the two commands to agree, and would keep passing if that
# policy changed, precisely because neither command has its own second source.
#
# THE PROPERTIES, and why a source-token scan alone would not be enough:
#
#   1. STRUCTURAL. tools/vms_monitor.c reaches the process list through
#      vms_kif_procscan() and contains no per-PID /proc enumeration; and
#      src/vmsdcl/dcl_cmd_show.c reads the SAME symbol. One source, named.
#
#   2. BEHAVIOURAL CONSISTENCY. Run both real binaries and compare the PID
#      sets they print. They must be EQUAL. Off the runtime (host CI, no
#      /dev/vms) both sets are empty -- which is itself the consistent state,
#      the same one SHOW SYSTEM already produces there -- so equality holds
#      trivially, and property 3 keeps that from being a vacuous pass.
#
#   3. THE EXECUTIVE WAS ACTUALLY READ. An empty MONITOR list is only honest
#      if MONITOR TRIED the executive and got nothing -- not if it silently
#      skipped to an empty/fabricated list. MONITOR's only /dev/vms touch is
#      the procscan bind, so a real openat("/dev/vms", ...) syscall (observed
#      with strace) is proof it read the executive rather than inventing the
#      list from somewhere else. A mutant that walks /proc instead never opens
#      /dev/vms at all -- see tests/integration/test_monitor_process_source_negctl.sh.
#      If strace is unavailable this property is reported FAILED, never
#      silently skipped (CLAUDE.md Rule 10, same convention as the SHOW DEVICE
#      gate).
#
# Usage: test_monitor_process_source.sh [PATH_TO_MONITOR.EXE] [PATH_TO_DCL.EXE]

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC_ROOT=$(cd "$HERE/../.." && pwd)

MONITOR="${1:-}"
DCL="${2:-}"
if [ -z "$MONITOR" ]; then
    for cand in "$SRC_ROOT/build/bin/MONITOR.EXE"; do
        [ -x "$cand" ] && MONITOR="$cand" && break
    done
fi
if [ -z "$DCL" ]; then
    for cand in "$SRC_ROOT/build/bin/DCL.EXE" "$SRC_ROOT/build/bin/vmsdcl"; do
        [ -x "$cand" ] && DCL="$cand" && break
    done
fi

MON_SRC="$SRC_ROOT/tools/vms_monitor.c"
SHOW_SRC="$SRC_ROOT/src/vmsdcl/dcl_cmd_show.c"

status=0
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM
STRACE=$(command -v strace 2>/dev/null || true)

echo "vms-c840 gate: MONITOR PROCESS and SHOW SYSTEM share one source (the executive process table)"

fail() {
    echo "FAIL: $1"
    shift
    for l in "$@"; do echo "  -> $l"; done
    status=1
}

# --- Property 1: STRUCTURAL --------------------------------------------------
# MONITOR sources the process list from vms_kif_procscan() and keeps NO
# independent per-PID /proc scan; SHOW SYSTEM reads the same symbol.
check_structural() {
    if [ ! -f "$MON_SRC" ]; then
        fail "tools/vms_monitor.c not found at $MON_SRC"
        return
    fi
    if grep -q 'vms_kif_procscan' "$MON_SRC"; then
        echo "  OK: MONITOR reads the process list via vms_kif_procscan()"
    else
        fail "MONITOR does not call vms_kif_procscan()" \
             "the process list must come from the executive table, the same" \
             "source SHOW SYSTEM uses -- not from a source of MONITOR's own"
    fi

    # Per-PID /proc enumeration tokens. The aggregate system counters MONITOR
    # legitimately keeps -- /proc/stat, /proc/meminfo, /proc/diskstats -- are
    # NOT process enumeration and are deliberately not matched here. What must
    # be gone is any walk of the process directory: a "/proc/%d/..." or
    # "/proc/<pid>/..." path, or an opendir() of /proc.
    if grep -Eq '/proc/%d|/proc/%i|opendir[[:space:]]*\([[:space:]]*"/proc"' "$MON_SRC"; then
        fail "MONITOR still enumerates processes from /proc" \
             "found a per-PID /proc scan -- that is the second, divergent" \
             "source this fix removes. The offending lines:" \
             "$(grep -nE '/proc/%d|/proc/%i|opendir[[:space:]]*\([[:space:]]*"/proc"' "$MON_SRC" | sed 's/^/       | /')"
    else
        echo "  OK: MONITOR keeps no per-PID /proc process scan"
    fi

    if [ -f "$SHOW_SRC" ] && grep -q 'vms_kif_procscan' "$SHOW_SRC"; then
        echo "  OK: SHOW SYSTEM reads the SAME source (vms_kif_procscan)"
    else
        fail "SHOW SYSTEM does not read vms_kif_procscan in $SHOW_SRC" \
             "the consistency this gate asserts depends on both commands" \
             "reading the one executive process table"
    fi
}

# --- PID-set extraction ------------------------------------------------------
# MONITOR PROCESS row: "%-20s  %08X  %-6s  ...", so the PID is the first
# 8-hex-digit token on a data row (the name column has no spaces in a VMS
# process name; an unnamed row is leading blanks then the PID). Strip the
# refresh loop's ANSI cursor/clear codes first.
monitor_pids() {
    timeout 8 "$MONITOR" PROCESS </dev/null 2>/dev/null \
        | sed 's/\x1b\[[0-9;]*[A-Za-z]//g; s/\x1b\[[HJ]//g' \
        | awk '{ for (i=1;i<=NF;i++) if ($i ~ /^[0-9A-Fa-f]{8}$/) { print toupper($i); break } }' \
        | sort -u
}

# SHOW SYSTEM row: "%08X %-15s ...", PID is the first field with no leading
# space. Header/banner lines never begin with an 8-hex token.
show_system_pids() {
    printf 'SHOW SYSTEM\n' | "$DCL" 2>/dev/null \
        | awk '$1 ~ /^[0-9A-Fa-f]{8}$/ { print toupper($1) }' \
        | sort -u
}

# --- Property 2: BEHAVIOURAL CONSISTENCY ------------------------------------
check_consistency() {
    if [ ! -x "$MONITOR" ]; then
        fail "MONITOR.EXE not found (looked at '$MONITOR')" \
             "this gate must RUN the command; it cannot be evaluated by reading"
        return
    fi
    if [ ! -x "$DCL" ]; then
        fail "DCL.EXE not found (looked at '$DCL')" \
             "the gate compares MONITOR's set against SHOW SYSTEM's"
        return
    fi

    monitor_pids > "$WORK/mon_pids"
    show_system_pids > "$WORK/sys_pids"

    if diff -u "$WORK/sys_pids" "$WORK/mon_pids" > "$WORK/pid_diff" 2>&1; then
        n=$(awk 'END{print NR}' "$WORK/mon_pids")
        echo "  OK: MONITOR PROCESS and SHOW SYSTEM list the SAME $n PID(s)"
    else
        fail "MONITOR PROCESS and SHOW SYSTEM listed DIFFERENT process sets" \
             "the two commands must read one source and agree. diff" \
             "(< SHOW SYSTEM, > MONITOR):" \
             "$(sed 's/^/       | /' "$WORK/pid_diff")"
    fi

    # If a real executive is present, the shared set must be non-empty (at
    # least the caller is registered). Off the runtime both are empty, which
    # is the consistent state SHOW SYSTEM produces there too.
    if [ -c /dev/vms ]; then
        if [ -s "$WORK/mon_pids" ]; then
            echo "  OK: executive present -- the shared process set is non-empty"
        else
            fail "an executive is present but both commands listed no process" \
                 "at least the caller should register in the executive table"
        fi
    else
        echo "  (no executive: /dev/vms absent -- both sets empty, the consistent state)"
    fi
}

# --- Property 3: MONITOR ACTUALLY READ THE EXECUTIVE ------------------------
# MONITOR's only /dev/vms interaction is the procscan bind, so a real
# openat("/dev/vms", ...) proves it read the executive rather than fabricating
# an empty (or any) list from another source. kif_bind() re-attempts the open
# on every procscan call because a failed open leaves the descriptor negative,
# so the syscall is observable whether or not the executive answers -- exactly
# the anchor the SHOW DEVICE gate uses one door further in.
check_executive_read_attempted() {
    if [ ! -x "$MONITOR" ]; then
        return   # already failed in property 2
    fi
    if [ -z "$STRACE" ]; then
        fail "strace is not available" \
             "property 3 (MONITOR actually read the executive) cannot be" \
             "evaluated without it, so it is reported FAILED, never skipped (Rule 10)"
        return
    fi
    timeout 8 "$STRACE" -f -e trace=openat -o "$WORK/strace.out" \
        "$MONITOR" PROCESS </dev/null >/dev/null 2>"$WORK/strace.err"
    if grep -q '"/dev/vms"' "$WORK/strace.out" 2>/dev/null; then
        echo "  OK: MONITOR issued openat(\"/dev/vms\", ...) -- it read the executive, not another source"
    else
        fail "MONITOR did not open /dev/vms -- it did not read the executive" \
             "an empty or independent process list that never touched" \
             "/dev/vms is the /proc-scan regression this gate exists to catch." \
             "openat() calls strace saw:" \
             "$(grep -F 'openat(' "$WORK/strace.out" 2>/dev/null | tail -8 | sed 's/^/       | /')"
    fi
}

check_structural
check_consistency
check_executive_read_attempted

if [ "$status" -eq 0 ]; then
    echo "vms-c840 gate: PASS"
else
    echo "vms-c840 gate: FAIL"
fi
exit $status
