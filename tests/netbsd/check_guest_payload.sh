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
# Every such file MUST appear as a SOURCE argument of some `COPY ...' line
# (single-source `COPY <path> <dest-file>' OR multi-source
# `COPY <path1> <path2> ... <dest-dir>/', vms-e7d-sibling: the Dockerfile
# consolidated 150 one-file-per-layer COPYs into 35 multi-source ones to stay
# under buildx's --load layer-depth ceiling; staged_files() below tokenizes
# EVERY COPY line's argument list rather than string-matching against the raw
# line, so a file's position among the sources on a shared line never matters.
#
# `selftest' (no repo files touched, no Dockerfile read) exercises
# staged_files()/is_staged() against synthetic fixture Dockerfiles: a
# multi-source COPY where the file under test is NOT the first source (must
# be found -- the regression this rewrite fixes) and a COPY set that genuinely
# omits a file (must still be reported missing -- the control must keep its
# teeth). Run it with `check_guest_payload.sh selftest'.
set -eu

REPO="${OVMX_REPO:-$(cd "$(dirname "$0")/../.." && pwd)}"
KMOD="$REPO/src/kernel-netbsd"
CORE="$REPO/src/kernel-core"
DF="$REPO/tests/netbsd/Dockerfile"
MK="$KMOD/Makefile"

# resolve a basename to its repo-relative path in one of the two module dirs
resolve() {
    if   [ -f "$KMOD/$1" ]; then echo "src/kernel-netbsd/$1"
    elif [ -f "$CORE/$1" ]; then echo "src/kernel-core/$1"
    fi
}

# staged_files <dockerfile> - every SOURCE argument of every `COPY ...' line in
# <dockerfile>, one per output line. Handles both COPY forms uniformly: for
# `COPY a b c dest/' (multi-source, dest is a directory) that is a, b, c; for
# `COPY a dest-file' (single-source) that is just a. Field-splits on
# whitespace and drops the LAST field on each COPY line (the destination) --
# never string-matches a source against the raw line, so a source's POSITION
# on the line (first, middle, last-before-dest) cannot hide it from this scan.
staged_files() {
    awk '
        /^COPY[[:space:]]/ {
            n = NF
            for (i = 2; i < n; i++) print $i
            next
        }
    ' "$1"
}

# is_staged <file> <staged-set-file> - exact-match membership test. Exact
# (grep -F -x against one-per-line source args), not a substring/regex test:
# a source path must equal <file> byte-for-byte, so "src/kernel-core/vms_ast.c"
# never false-matches "src/kernel-core/vms_ast_secmode.c" or similar.
is_staged() {
    grep -qxF "$1" "$2"
}

cmd_check() {
    [ -f "$DF" ] || { echo "FAIL: $DF not found"; exit 2; }
    [ -f "$MK" ] || { echo "FAIL: $MK not found"; exit 2; }

    # 1. the .c the module Makefile builds -- the SRCS= assignment, which spans
    #    backslash-continued lines. Take from `SRCS=' up to the first line NOT
    #    ending in a backslash, then pull every `<name>.c' token.
    srcs_block="$(awk '/^SRCS[[:space:]]*=/{f=1}
                       f{print}
                       f && !/\\[[:space:]]*$/{exit}' "$MK")"
    srcs="$(printf '%s\n' "$srcs_block" | grep -oE '[A-Za-z0-9_]+\.c' | sort -u)"
    [ -n "$srcs" ] || { echo "FAIL: could not parse SRCS from $MK"; exit 2; }

    # 2. transitive local-header closure over the module files (seed: the SRCS
    #    .c + the aggregation header vms_internal.h).
    worklist=""
    for c in $srcs; do r="$(resolve "$c")"; [ -n "$r" ] && worklist="$worklist $r"; done
    worklist="$worklist src/kernel-netbsd/vms_internal.h"

    closure=""
    add() { case " $closure " in *" $1 "*) ;; *) closure="$closure $1"; worklist="$worklist $1";; esac; }
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

    # 3. assert each needed file is a COPY source somewhere in the Dockerfile.
    staged_tmp="$(mktemp)"
    trap 'rm -f "$staged_tmp"' EXIT
    staged_files "$DF" >"$staged_tmp"

    missing=0
    for f in $need; do
        if ! is_staged "$f" "$staged_tmp"; then
            echo "FAIL: $f is needed by the in-guest module build but is NOT staged by tests/netbsd/Dockerfile"
            missing=1
        fi
    done

    if [ "$missing" -ne 0 ]; then
        echo "  -> add the missing file to an existing 'COPY ... /netbsd/guest-src/...' line's"
        echo "     source list (or a new COPY line); the QEMU executive jobs build the module"
        echo "     in-guest from exactly this payload."
        exit 1
    fi
    echo "PASS: tests/netbsd/Dockerfile stages every source + local header the in-guest module build needs ($(printf '%s\n' $need | wc -l | tr -d ' ') files)"
}

cmd_selftest() {
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    rc=0

    # Fixture A: a multi-source COPY where the file under test is the LAST
    # source before the destination directory (the position most likely to be
    # missed by a naive "first token after COPY" parse -- the exact regression
    # a live CI run caught, vms-e7d: exec_kbackend.h etc. reported missing
    # after the Dockerfile's 150 one-file COPYs were consolidated to 35).
    cat >"$tmp/multi.Dockerfile" <<'EOF'
FROM scratch
COPY src/kernel-core/vms_eflag.c src/kernel-core/exec_kbackend.h src/kernel-core/exec_list.h /netbsd/guest-src/kernel-core/
EOF
    staged_files "$tmp/multi.Dockerfile" >"$tmp/multi.staged"
    if is_staged "src/kernel-core/exec_kbackend.h" "$tmp/multi.staged"; then
        echo "  ok: staged_files() finds a MIDDLE source on a multi-source COPY line"
    else
        echo "FAIL: staged_files() missed src/kernel-core/exec_kbackend.h, a middle source"
        echo "      on a multi-source COPY line -- this is the exact vms-e7d regression"
        rc=1
    fi
    if is_staged "src/kernel-core/exec_list.h" "$tmp/multi.staged"; then
        echo "  ok: staged_files() finds the LAST source (immediately before the dest dir)"
    else
        echo "FAIL: staged_files() missed src/kernel-core/exec_list.h, the last source before"
        echo "      the destination directory -- it is being read as part of the dest"
        rc=1
    fi
    if is_staged "src/kernel-core/vms_eflag.c" "$tmp/multi.staged"; then
        echo "  ok: staged_files() still finds the FIRST source (the pre-fix parse's only case)"
    else
        echo "FAIL: staged_files() lost the FIRST source on a multi-source COPY line"
        rc=1
    fi
    # the destination directory itself must never be reported as staged
    if is_staged "/netbsd/guest-src/kernel-core/" "$tmp/multi.staged"; then
        echo "FAIL: staged_files() reported the DESTINATION directory as a staged source"
        rc=1
    else
        echo "  ok: staged_files() does not mistake the destination for a source"
    fi

    # Fixture B: the control must still have teeth -- a file genuinely absent
    # from every COPY line (single- or multi-source) must still be reported
    # missing, not silently swallowed by the wider multi-source parse.
    cat >"$tmp/incomplete.Dockerfile" <<'EOF'
FROM scratch
COPY src/kernel-core/vms_eflag.c src/kernel-core/exec_kbackend.h /netbsd/guest-src/kernel-core/
COPY src/kernel-netbsd/vms_netbsd.c /netbsd/guest-src/kmod/vms_netbsd.c
EOF
    staged_files "$tmp/incomplete.Dockerfile" >"$tmp/incomplete.staged"
    if is_staged "src/kernel-core/vms_ast.c" "$tmp/incomplete.staged"; then
        echo "FAIL: is_staged() reported a genuinely-absent file as staged -- the control has"
        echo "      no teeth"
        rc=1
    else
        echo "  ok: a genuinely-unstaged file is still correctly reported absent"
    fi
    # exact-match: a file whose path is a SUBSTRING of a staged one must not
    # false-positive (vms_ast.c vs. a hypothetical vms_ast_secmode.c neighbor).
    cat >"$tmp/substr.Dockerfile" <<'EOF'
FROM scratch
COPY src/kernel-core/vms_ast_secmode.c /netbsd/guest-src/kernel-core/vms_ast_secmode.c
EOF
    staged_files "$tmp/substr.Dockerfile" >"$tmp/substr.staged"
    if is_staged "src/kernel-core/vms_ast.c" "$tmp/substr.staged"; then
        echo "FAIL: is_staged() substring-matched src/kernel-core/vms_ast.c against a staged"
        echo "      neighbor (vms_ast_secmode.c) instead of requiring an exact path match"
        rc=1
    else
        echo "  ok: is_staged() requires an exact path match, not a substring"
    fi

    if [ "$rc" -eq 0 ]; then
        echo "PASS: staged_files()/is_staged() correctly parse both COPY forms"
    fi
    return "$rc"
}

case "${1:-}" in
    selftest) shift; cmd_selftest "$@";;
    "")       cmd_check;;
    *)        echo "usage: check_guest_payload.sh [selftest]" >&2; exit 2;;
esac
