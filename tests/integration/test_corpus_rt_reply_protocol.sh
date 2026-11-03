#!/bin/bash
# test_corpus_rt_reply_protocol.sh - the corpus runtime's reply protocol cannot
# lose sync with the boot task (rd vms-84e)
#
# CI run 41588156228 reported "CORPUS-RT sys_cef rc= signaled=0": an EMPTY rc,
# and the scoreboard counted a run-pass program as run-fail. The program had
# not failed; init.sh had lost its place in the boot task's reply FIFO. It
# enforced the 40 s budget by polling the FIFO with `read -t 1`, and a read
# with a timeout DISCARDS what it has read when the deadline lapses mid-line
# (busybox reads a byte at a time and re-checks the deadline before each one;
# bash keeps the partial words but the loop re-reads). The next read parsed the
# tail of "RC 0" -- "0" as the tag, nothing as the rc.
#
# The fix moves the budget into the boot task (RUNB, tests/qemu/
# ovmx_wait_budget.h) and leaves init.sh only blocking reads of whole lines.
#
# These controls EXECUTE the code, no QEMU:
#   1. init.sh's real corpus loop (cut out of init.sh, run under every shell
#      present: bash, and busybox -- the guest's own shell -- when the host has it) against a fake boot task that delivers the
#      RC line in two pieces with a 1.5 s gap -- the condition that desynced
#      it -- must report the exact rc. Its DISPROOF runs the old polling loop on
#      the same input and asserts it gets it wrong.
#   2. the boot task's budgeted wait (ovmx_wait_budget.h, compiled here) reports
#      exit codes, 128+signal, and 124 for a program it killed at its budget.

set -uo pipefail

ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
INIT="$ROOT/tests/qemu/init.sh"
HDR="$ROOT/tests/qemu/ovmx_wait_budget.h"

passed=0
failed=0
ok()  { echo "  PASS: $1"; passed=$((passed + 1)); }
bad() { echo "  FAIL: $1"; failed=$((failed + 1)); }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# --- fake boot task: answers RUN/RUNB like ovmx_boot_task, but splits the RC
# line ("RC 1" ... 1.5 s ... "23\n") across a gap longer than the old poll.
cat >"$WORK/fake_boot_task.sh" <<'EOF'
#!/bin/bash
ctl=$1 reply=$2
exec 5<>"$ctl" 6<>"$reply"
while read -r verb rest <&5; do
    case "$verb" in
        EXIT) exit 0 ;;
        RUN|RUNB) ;;
        *) continue ;;
    esac
    printf 'PID 4242 1\n' >&6
    printf 'RC 1' >&6
    sleep 1.5
    printf '23\n' >&6
done
EOF
chmod +x "$WORK/fake_boot_task.sh"

# --- the corpus loop exactly as init.sh has it, from its header comment to
# the DONE line's closing `fi`, pointed at a scratch program directory.
CRT="$WORK/corpus_rt"
mkdir -p "$CRT"
printf '#!/bin/sh\nexit 0\n' >"$CRT/prog_a"
chmod +x "$CRT/prog_a"
loop=$(awk '/^# CORPUS RUNTIME LOOP/{on=1} on{print} on&&/CORPUS-RT DONE/{d=1} d&&/^fi$/{exit}' "$INIT")
if [ -z "$loop" ] || ! printf '%s\n' "$loop" | grep -q 'CORPUS-RT \$name rc='; then
    bad "could not cut the corpus loop out of $INIT -- this control no longer tests it"
    echo "=== $passed passed, $failed failed ==="
    exit 1
fi
loop=${loop//\/tests\/corpus_rt\//$CRT/}
loop=${loop//\/tmp\/corpus_cwd/$WORK/cwd}

# The pre-fix loop body (verbatim reads), the disproof's subject.
old_loop='
for prog in '"$CRT"'/*; do
    name=$(basename "$prog")
    _t=0
    echo "RUN $prog /dev/null" >&8
    read -r _tag _pp _pst <&7
    while ! read -t 1 -r _tag rc <&7; do
        _t=$((_t+1))
        if [ "$_t" -ge 40 ]; then read -r _tag rc <&7; break; fi
    done
    echo "CORPUS-RT $name rc=$rc signaled=0" >&4
done'

run_under() {   # <shell...> -- <loop text>; prints the CORPUS-RT lines
    local sh=("${@:1:$#-1}") body="${*: -1}"
    local d; d=$(mktemp -d "$WORK/run.XXXX")
    mkfifo "$d/ctl" "$d/reply"
    "$WORK/fake_boot_task.sh" "$d/ctl" "$d/reply" &
    local fbt=$!
    cat >"$d/drive.sh" <<EOF
exec 7<>"$d/reply" 8<>"$d/ctl" 4>&1
BOOT_OK=1 CORPUS_RT=1 CORPUS_SKIP=
$body
echo EXIT >&8
EOF
    "${sh[@]}" "$d/drive.sh" 2>&1 | grep '^CORPUS-RT '
    wait "$fbt" 2>/dev/null
}

shells=("bash")
command -v busybox >/dev/null 2>&1 && shells+=("busybox sh")

echo "--- control 1: a split RC line reaches init.sh's corpus loop whole"
for s in "${shells[@]}"; do
    # shellcheck disable=SC2086
    got=$(run_under $s "$loop")
    if [ "$got" = "CORPUS-RT prog_a rc=123 signaled=0" ]; then
        ok "[$s] init.sh corpus loop reads rc=123 across a 1.5 s split"
    else
        bad "[$s] init.sh corpus loop: expected 'CORPUS-RT prog_a rc=123 signaled=0', got '$got'"
    fi
    # shellcheck disable=SC2086
    old=$(run_under $s "$old_loop")
    case "$old" in
        *"rc=123 "*) bad "[$s] DISPROOF: the old read -t loop read the split line correctly -- control 1 no longer reproduces the desync" ;;
        *) ok "[$s] disproof: the old read -t loop desyncs on the same input ('$old')" ;;
    esac
done
if printf '%s\n' "$loop" | grep -v '^[[:space:]]*#' | grep -q 'read -t'; then
    bad "init.sh's corpus loop reads the reply FIFO with a timeout again"
else
    ok "init.sh's corpus loop has only blocking reads of the reply FIFO"
fi

echo "--- control 2: the boot task's budgeted wait"
cat >"$WORK/drv.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "ovmx_wait_budget.h"
int main(int argc, char **argv)
{
    pid_t p = fork();
    if (p == 0) { execvp(argv[2], argv + 2); _exit(127); }
    printf("%d\n", ovmx_wait_rc(p, (unsigned)atoi(argv[1])));
    return 0;
}
EOF
CC_BIN="${CC:-cc}"
if ! "$CC_BIN" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Werror -I"$(dirname "$HDR")" \
        -o "$WORK/drv" "$WORK/drv.c"; then
    bad "ovmx_wait_budget.h does not compile on the host"
else
    expect() {   # <want> <max-seconds> <budget> <cmd...>
        local want=$1 max=$2; shift 2
        local t0 t1 got
        t0=$(date +%s)
        got=$("$WORK/drv" "$@")
        t1=$(date +%s)
        if [ "$got" = "$want" ] && [ $((t1 - t0)) -le "$max" ]; then
            ok "budget $1: '${*:2}' -> rc $got in $((t1 - t0))s"
        else
            bad "budget $1: '${*:2}' -> rc '$got' in $((t1 - t0))s (want $want within ${max}s)"
        fi
    }
    expect 3   5 0 sh -c 'exit 3'
    expect 143 5 0 sh -c 'kill -TERM $$'
    expect 0   5 40 sh -c 'exit 0'
    expect 124 5 1 sleep 30
fi

echo "=== $passed passed, $failed failed ==="
[ "$failed" -eq 0 ]
