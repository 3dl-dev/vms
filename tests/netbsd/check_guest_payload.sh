#!/usr/bin/env bash
# check_guest_payload.sh (rd vms-72da) -- assert tests/netbsd/Dockerfile STAGES
# every source + local header the in-guest QEMU executive-module build needs.
#
# WHY THIS EXISTS. The QEMU harness (tests/netbsd/drive_netbsd_p2b.py /
# drive_netbsd_p4a.py) builds the `vms' module IN-GUEST from a HAND-CURATED
# per-file COPY payload baked by tests/netbsd/Dockerfile (COPY ... ->
# /netbsd/guest-src/kmod + /netbsd/guest-src/kernel-core). The per-PR
# cross-compile gates (crosscompile.sh, the vax build-*.sh) build from the REAL
# src tree, so they CANNOT catch a file the Makefile/headers need but the
# Dockerfile forgot to stage. That gap let vms-72da add vms_lnm_nb.h /
# vms_lnm_arena_netbsd.c / vms_lnm.c to the module without staging them, reddening
# only the QEMU executive jobs (and nightly) with
#   vms_internal.h:81: fatal error: vms_lnm_nb.h: No such file or directory
# This check is the negative control for the hand-curated payload: it FAILS on
# the PR the moment the payload drifts from what the module build needs.
#
# WHAT IT CHECKS. The set the in-guest build needs = every .c in the module
# Makefile's SRCS (bare names -> src/kernel-netbsd/, .PATH ../kernel-core names ->
# src/kernel-core/) PLUS the transitive closure of local `#include "..."' headers
# that live in those two dirs, starting from those .c and from vms_internal.h.
# Every such file MUST appear in a `COPY <path> /netbsd/guest-src/...' line.
set -eu

REPO="${OVMX_REPO:-$(cd "$(dirname "$0")/../.." && pwd)}"
KMOD="$REPO/src/kernel-netbsd"
CORE="$REPO/src/kernel-core"
DF="$REPO/tests/netbsd/Dockerfile"
MK="$KMOD/Makefile"

[ -f "$DF" ] || { echo "FAIL: $DF not found"; exit 2; }
[ -f "$MK" ] || { echo "FAIL: $MK not found"; exit 2; }

# resolve a basename to its repo-relative path in one of the two module dirs
resolve() {
    if   [ -f "$KMOD/$1" ]; then echo "src/kernel-netbsd/$1"
    elif [ -f "$CORE/$1" ]; then echo "src/kernel-core/$1"
    fi
}

# 1. the .c the module Makefile builds -- the SRCS= assignment, which spans
#    backslash-continued lines. Take from `SRCS=' up to the first line NOT ending
#    in a backslash, then pull every `<name>.c' token.
srcs_block="$(awk '/^SRCS[[:space:]]*=/{f=1}
                   f{print}
                   f && !/\\[[:space:]]*$/{exit}' "$MK")"
srcs="$(printf '%s\n' "$srcs_block" | grep -oE '[A-Za-z0-9_]+\.c' | sort -u)"
[ -n "$srcs" ] || { echo "FAIL: could not parse SRCS from $MK"; exit 2; }

# 2. transitive local-header closure over the module files (seed: the SRCS .c +
#    the aggregation header vms_internal.h).
worklist=""
for c in $srcs; do r="$(resolve "$c")"; [ -n "$r" ] && worklist="$worklist $r"; done
worklist="$worklist src/kernel-netbsd/vms_internal.h"

closure=""
add() { case " $closure " in *" $1 "*) ;; *) closure="$closure $1"; worklist="$worklist $1";; esac; }
# prime the closure with the worklist seed
for f in $worklist; do add "$f"; done

# BFS over #include "..." edges, resolving to the two dirs only
pending="$closure"
while [ -n "$(printf '%s' "$pending" | tr -d ' ')" ]; do
    next=""
    for f in $pending; do
        incs="$(grep -hoE '#[[:space:]]*include[[:space:]]*"[^"]+"' "$REPO/$f" 2>/dev/null \
                | sed -E 's/.*"([^"]+)".*/\1/')"
        for h in $incs; do
            b="$(basename "$h")"
            r="$(resolve "$b")"; [ -z "$r" ] && continue
            case " $closure " in
                *" $r "*) ;;
                *) closure="$closure $r"; next="$next $r";;
            esac
        done
    done
    pending="$next"
done

need="$(printf '%s\n' $srcs | while read -r c; do resolve "$c"; done; printf '%s\n' $closure)"
need="$(printf '%s\n' $need | sort -u | grep .)"

# 3. assert each needed file is COPY'd into the guest payload by the Dockerfile.
missing=0
for f in $need; do
    if ! grep -qE "^COPY[[:space:]]+$f([[:space:]]|\$)" "$DF"; then
        echo "FAIL: $f is needed by the in-guest module build but is NOT staged by tests/netbsd/Dockerfile"
        missing=1
    fi
done

if [ "$missing" -ne 0 ]; then
    echo "  -> add the missing 'COPY <path> /netbsd/guest-src/...' line(s); the QEMU"
    echo "     executive jobs build the module in-guest from exactly this payload."
    exit 1
fi
echo "PASS: tests/netbsd/Dockerfile stages every source + local header the in-guest module build needs ($(printf '%s\n' $need | wc -l | tr -d ' ') files)"
exit 0
