#!/bin/bash
# test_dnet_ncp_object.sh - NCP.EXE OBJECT command dispatch, end-to-end
# (rd vms-f52). The first command-dispatch test for NCP.EXE: it drives the real
# shipped binary (SET/DEFINE/SHOW/CLEAR/PURGE OBJECT) against a temp object
# database via OVMX_DECNET_OBJECTDB, asserting behaviour, persistence across
# invocations, and honest errors. No socket, no privilege, no /dev/vms.
set -uo pipefail

NCP="${NCP_EXE:-}"
if [ -z "$NCP" ] || [ ! -x "$NCP" ]; then
    NCP="$(dirname "$0")/../../build/bin/NCP.EXE"
fi
if [ ! -x "$NCP" ]; then
    echo "FAIL: NCP.EXE not found (set NCP_EXE or build ncp_exe first)"; exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export OVMX_DECNET_OBJECTDB="$TMP/object.dat"

PASS=0 FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
# run NCP, capture stdout+stderr + rc
ncp() { "$NCP" "$@" 2>&1; }

# --- SET OBJECT persists; SHOW sees it (a SEPARATE invocation => real DB) ------
ncp SET OBJECT CTERM NUMBER 42 FILE SYS\$SYSTEM:DECNETD.EXE >/dev/null; rc=$?
[ "$rc" -eq 0 ] && ok "SET OBJECT CTERM NUMBER 42 FILE ... exits 0" \
                || bad "SET OBJECT CTERM exit $rc"
out="$(ncp SHOW OBJECT CTERM)"
echo "$out" | grep -q "42" && echo "$out" | grep -q "DECNETD.EXE" \
    && ok "SHOW OBJECT CTERM (new invocation) shows number 42 + file (persisted)" \
    || bad "SHOW OBJECT CTERM missing 42/file: $out"

ncp SET OBJECT FAL NUMBER 17 >/dev/null
ncp DEFINE OBJECT TASK NUMBER 25 >/dev/null
out="$(ncp SHOW KNOWN OBJECTS)"
# ascending-number order: 17 (FAL) before 25 (TASK) before 42 (CTERM)
if echo "$out" | grep -qE "FAL" && echo "$out" | grep -qE "TASK" && echo "$out" | grep -qE "CTERM"; then
    order="$(echo "$out" | grep -oE 'FAL|TASK|CTERM' | tr '\n' ' ')"
    [ "$order" = "FAL TASK CTERM " ] \
        && ok "SHOW KNOWN OBJECTS lists all in ascending-number order (FAL,TASK,CTERM)" \
        || bad "SHOW KNOWN OBJECTS wrong order: $order"
else
    bad "SHOW KNOWN OBJECTS missing entries: $out"
fi

# --- DEFINE updates an existing number (file added) ---------------------------
ncp DEFINE OBJECT FAL NUMBER 17 FILE SYS\$SYSTEM:DECNETD.EXE >/dev/null
out="$(ncp SHOW OBJECT 17)"
echo "$out" | grep -q "DECNETD.EXE" \
    && ok "DEFINE OBJECT updates FAL's file; SHOW OBJECT <number> reflects it" \
    || bad "update not reflected: $out"

# --- CLEAR by name, PURGE by number -------------------------------------------
ncp CLEAR OBJECT CTERM >/dev/null; rc=$?
[ "$rc" -eq 0 ] && ! ncp SHOW OBJECT CTERM >/dev/null 2>&1 \
    && ok "CLEAR OBJECT <name> removes it (SHOW then fails)" \
    || bad "CLEAR OBJECT CTERM did not remove it (rc=$rc)"
ncp PURGE OBJECT 17 >/dev/null; rc=$?
[ "$rc" -eq 0 ] && ! ncp SHOW OBJECT 17 >/dev/null 2>&1 \
    && ok "PURGE OBJECT <number> removes it" \
    || bad "PURGE OBJECT 17 did not remove it (rc=$rc)"

# --- honest errors (INV-6: refuse, never fabricate) ---------------------------
err() { # desc, expected-code, args...
    local desc="$1" want="$2"; shift 2
    local o rc; o="$(ncp "$@")"; rc=$?
    if [ "$rc" -ne 0 ] && echo "$o" | grep -q "$want"; then ok "$desc"; else bad "$desc (rc=$rc: $o)"; fi
}
err "SET OBJECT without NUMBER is refused"          "MISSOBJNUM" SET OBJECT NONUM
err "SET OBJECT with a bad number is refused"       "INVOBJNUM"  SET OBJECT X NUMBER 999
err "duplicate object name is refused"              "INVOBJ"     SET OBJECT TASK NUMBER 88
err "SHOW OBJECT of an unknown name is refused"     "UNROBJ"     SHOW OBJECT NOSUCH
err "CLEAR OBJECT of an unknown object is refused"  "UNROBJ"     CLEAR OBJECT NOSUCH

echo "----"; echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
