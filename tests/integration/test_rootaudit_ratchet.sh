#!/bin/bash
# test_rootaudit_ratchet.sh - the vms-251b substrate-root gate can go red.
#
# 1. ROOTAUDIT.EXE's census: run on this (unprivileged-or-not) host, it must
#    name at least one root process (the host's own init is one), and report
#    the same count in its summary.
# 2. The battery's ratchet (rootaudit_check in tests/qemu/lib/
#    dcl_acceptance_battery.sh) over canned ROOTAUDIT output:
#      - exactly the known processes            -> no FAIL
#      - an extra, unlisted root process         -> FAIL (a new root process)
#      - a listed process no longer running root -> FAIL (stale entry)
#      - no SUMMARY line (ROOTAUDIT did not run) -> FAIL
# usage: test_rootaudit_ratchet.sh <ROOTAUDIT.EXE> <repo-root>
set -u
RA="${1:?ROOTAUDIT.EXE}"; REPO="${2:?repo root}"
fails=0
die() { echo "FAIL: $1"; fails=$((fails + 1)); }

out=$("$RA")
n=$(printf '%s\n' "$out" | grep -c 'SUBSTRATEROOT, pid ' || true)
s=$(printf '%s\n' "$out" | sed -n 's/.*%ROOTAUDIT-I-SUMMARY, \([0-9]*\) process.*/\1/p')
[ "${n:-0}" -ge 1 ] || die "ROOTAUDIT named no root process on a host whose init is root"
[ "$s" = "$n" ] || die "ROOTAUDIT summary ($s) disagrees with the processes it named ($n)"

# Source only the battery's helpers and the ratchet, then score segments.
PASS=0; FAIL=0
# shellcheck disable=SC1091
source "$REPO/tests/qemu/lib/dcl_acceptance_battery.sh"
EXPECTED_ARCH_NAME=X86_64
seg() {   # <names...> -> a ROOTAUDIT console segment naming them as root
    printf '%%ROOTAUDIT-I-SUMMARY, %d process(es) run as substrate root\n' "$#"
    local i=1 x
    for x in "$@"; do printf '%%ROOTAUDIT-W-SUBSTRATEROOT, pid %d %s uid 0/0/0/0 capeff 0\n' $i "$x"; i=$((i+1)); done
}
score() { FAIL=0; rootaudit_check "$1" >/dev/null; echo "$FAIL"; }
known=$(for k in "${substrate_root_known[@]}"; do printf '%s ' "$(printf '%s' "$k" | awk '{print $2}')"; done)
# shellcheck disable=SC2086
[ "$(score "$(seg $known)")" = 0 ] || die "the exactly-known set was scored red"
# shellcheck disable=SC2086
[ "$(score "$(seg $known EVIL.EXE)")" -ge 1 ] || die "an unlisted root process was not caught"
if [ -n "$known" ]; then
    # shellcheck disable=SC2086
    set -- $known; shift
    # shellcheck disable=SC2068
    [ "$(score "$(seg $@)")" -ge 1 ] || die "a stale substrate_root_known entry was not caught"
fi
[ "$(score "nothing ran")" -ge 1 ] || die "a segment with no ROOTAUDIT output was scored green"

[ "$fails" -eq 0 ] && echo "PASS: ROOTAUDIT names root processes and the ratchet goes red on a new or stale one" && exit 0
exit 1
