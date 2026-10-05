#!/bin/bash
#
# corpus_runtime_report.sh - the corpus RUNTIME scoreboard column (vms-44a, R2.3).
#
# Reads the output of `OVMX_CORPUS_RT=1 tests/qemu/run_tests.sh` (the guest ran
# every tier-1 program in tests/qemu/corpus_runtime_programs.txt under a live
# vms.ko / /dev/vms), writes a machine-readable report to stdout, and -- when a
# baseline is given -- fails on a regression:
#
#   * a program that was run-pass in the baseline and is not now, or
#   * a run-pass count below the baseline's floor, or
#   * a zero-program run (vacuity: a guest that ran nothing is never a pass).
#
# Usage: corpus_runtime_report.sh <qemu-output-file> [baseline.json] [programs.txt]
#
# Statuses (mirror tests/conformance/run_corpus.sh, plus the two a guest adds):
#   run-pass    exit 0
#   run-fail    exit non-zero (rc 124 = the 10s guest budget expired)
#   run-crash   killed by a signal (rc > 128)
#   vm-crash    the program took the whole guest down (BEGIN line, no result)
#   not-run     in the program list, never reached (guest died earlier / absent binary)
# "signaled" marks a pass that printed an unhandled %FAC-E-/-F- condition.
set -euo pipefail

OUT=${1:?usage: corpus_runtime_report.sh <qemu-output-file> [baseline.json] [programs.txt]}
BASELINE=${2:-}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIST=${3:-$HERE/corpus_runtime_programs.txt}

declare -A rc sig began expected_exit
# designed non-zero exits (tests/corpus/expected_exit.txt): run-pass iff exactly that code
EXPECTED_EXIT_FILE="$HERE/../corpus/expected_exit.txt"
if [ -f "$EXPECTED_EXIT_FILE" ]; then
    while read -r _en _ec _rest; do
        case "$_en" in ''|\#*) continue ;; esac
        expected_exit["$_en"]="$_ec"
    done < "$EXPECTED_EXIT_FILE"
fi
while IFS= read -r line; do
    line=${line%$'\r'}
    case "$line" in
    "CORPUS-RT-BEGIN "*) began["${line#CORPUS-RT-BEGIN }"]=1 ;;
    "CORPUS-RT "*)
        rest=${line#CORPUS-RT }
        name=${rest%% *}
        r=${rest#* rc=}; r=${r%% *}
        s=${rest##*signaled=}
        rc["$name"]=$r; sig["$name"]=$s ;;
    esac
done < "$OUT"

total=0; pass=0; fail=0; crash=0; vmcrash=0; notrun=0; signaled=0
progs=""
while IFS= read -r name; do
    case "$name" in ''|\#*) continue ;; esac
    total=$((total+1))
    if [ -n "${rc[$name]+x}" ]; then
        r=${rc[$name]}
        if [ "$r" -eq 0 ] || { [ -n "${expected_exit[$name]:-}" ] && [ "$r" -eq "${expected_exit[$name]}" ]; }; then st=run-pass; pass=$((pass+1)); [ "${sig[$name]}" = 1 ] && signaled=$((signaled+1))
        elif [ "$r" -gt 128 ]; then st=run-crash; crash=$((crash+1))
        else st=run-fail; fail=$((fail+1)); fi
    elif [ -n "${began[$name]+x}" ]; then st=vm-crash; vmcrash=$((vmcrash+1))
    else st=not-run; notrun=$((notrun+1)); fi
    progs="$progs{\"name\":\"$name\",\"status\":\"$st\",\"signaled\":$([ "${sig[$name]:-0}" = 1 ] && echo true || echo false)},"
done < "$LIST"
progs=${progs%,}

report=$(printf '{"total":%d,"summary":{"run-pass":%d,"run-fail":%d,"run-crash":%d,"vm-crash":%d,"not-run":%d,"run-pass-signaled":%d},"programs":[%s]}' \
    "$total" "$pass" "$fail" "$crash" "$vmcrash" "$notrun" "$signaled" "$progs")
printf '%s\n' "$report" | jq .

[ "$total" -ge 1 ] || { echo "::error::corpus runtime: program list is empty -- a zero-program run is never a pass" >&2; exit 2; }
ran=$((pass+fail+crash+vmcrash))
[ "$ran" -ge 1 ] || { echo "::error::corpus runtime: the guest ran no corpus program at all (see the QEMU output)" >&2; exit 2; }

if [ -n "$BASELINE" ]; then
    floor=$(jq '.summary."run-pass"' "$BASELINE")
    echo "corpus runtime: run-pass $pass / $total (committed floor $floor)" >&2
    bad=0
    if [ "$pass" -lt "$floor" ]; then
        echo "::error::corpus runtime REGRESSION -- run-pass $pass is below the committed floor $floor" >&2; bad=1
    fi
    for n in $(jq -r '.programs[]|select(.status=="run-pass")|.name' "$BASELINE"); do
        cur=$(printf '%s' "$report" | jq -r --arg n "$n" '.programs[]|select(.name==$n)|.status')
        if [ "$cur" != "run-pass" ]; then
            echo "::error::corpus runtime REGRESSION -- $n was run-pass in the baseline, now '${cur:-absent}'" >&2; bad=1
        fi
    done
    [ "$bad" -eq 0 ] || exit 1
fi
