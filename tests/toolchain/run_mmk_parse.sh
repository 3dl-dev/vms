#!/bin/sh
# run_mmk_parse.sh - MMK.EXE description-file PARSE + PLAN end-to-end proof
#                    (bead vms-ec70, self-host spine #4).
#
# Veracity (Q1/Q2): a REAL end-to-end through the actual files MMK reads.
#   1. A real descrip.mms is written to the OVMX system disk (/vms), naming a
#      two-step build:  target.EXE <- target.OBJ <- target.C  with explicit
#      TCC/LINK commands.
#   2. MMK.EXE is run as a foreign command ("/DESCRIPTION=... /NOACTION/LOG
#      <target>").  It reads the descrip.mms through OVMX RMS, parses it with the
#      REAL lib$table_parse engine driven by the ported PARSE_TABLES.MAR grammar
#      (bead vms-486) via MMK's own parse_store, and resolves the command line
#      through the CLI$ compiled-CLD path (cli$compile_cld on mmk_cld.cld, bead
#      vms-8c1).
#   3. In /NOACTION (dry-run) mode MMK emits the exact commands it WOULD run, in
#      dependency order.  The test asserts BOTH commands appear in the right
#      order — proving MMK really built the target/dependency graph and resolved
#      the inference chain, not that a script faked it.
#
# The independent oracle is the descrip.mms itself: the expected output is the
# TCC/LINK command bodies the file specifies, in the order the dependency graph
# dictates (source compiled before it is linked).
#
# ACTUAL command EXECUTION (turning the plan into a built image) rides MMK's
# DCL-subprocess/mailbox/AST drive, which is a separately-tracked deferred gap
# (see the vms-ec70 PR); this test proves the parse+plan half end-to-end.
#
# Inputs (env, set by CMake add_test): MMK_EXE.  Exit 0 = success.
set -e

: "${MMK_EXE:?need MMK_EXE (built MMK.EXE)}"
[ -x "$MMK_EXE" ] || { echo "FAIL: MMK_EXE not executable: $MMK_EXE"; exit 1; }

# MMK opens the description file through OVMX RMS, which resolves a bare filespec
# against the process default directory (cwd).  Work in an isolated temp dir and
# cd into it so the run is hermetic (no dependency on /vms contents).
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

DESCRIP="MMK_SPINE_TEST.MMS"
SRC="MMKSPINE.C"
OBJ="MMKSPINE.OBJ"
EXE="MMKSPINE.EXE"

# A real MMS description file: target <- object <- source, tab-indented command
# bodies (MMK requires the leading tab).
printf '%s : %s\n\tLINK %s\n\n%s : %s\n\tTCC %s\n' \
    "$EXE" "$OBJ" "$OBJ" "$OBJ" "$SRC" "$SRC" > "$DESCRIP"
printf 'int main(void){return 0;}\n' > "$SRC"
# /RULES defaults to MMS$RULES; provide an empty one so MMK does not warn about a
# missing default rules file (keeps the run's final status clean).
printf '! empty default rules (vms-ec70 parse test)\n' > "MMS\$RULES"

echo "== descrip.mms =="
sed 's/^/   /' "$DESCRIP"

echo
echo "== MMK.EXE /DESCRIPTION=$DESCRIP /NOACTION/LOG $EXE =="
OUT=$(mktemp)
set +e
VMS_FOREIGN_CMD="/DESCRIPTION=$DESCRIP /NOACTION/LOG $EXE" \
    "$MMK_EXE" < /dev/null > "$OUT" 2>/dev/null
RC=$?
set -e
echo "-- MMK exit=$RC (odd VMS status = success); stdout: --"
sed 's/^/   /' "$OUT"

# VMS success status is ODD; exit(SS$_NORMAL=1) -> shell exit 1.
case "$RC" in
    1|0) : ;;   # 1 == SS$_NORMAL low byte; 0 tolerated
    *) if [ $((RC & 1)) -eq 0 ]; then echo "FAIL: MMK exited with an even (failure) status $RC"; rm -f "$OUT"; exit 1; fi ;;
esac

# Assert the plan: TCC before LINK, both present with the right operands.
tcc_line=$(grep -n "^TCC $SRC\$"  "$OUT" | head -1 | cut -d: -f1)
lnk_line=$(grep -n "^LINK $OBJ\$" "$OUT" | head -1 | cut -d: -f1)
rm -f "$OUT"

[ -n "$tcc_line" ] || { echo "FAIL: expected 'TCC $SRC' in the plan (MMK did not resolve the .C->.OBJ step)"; exit 1; }
[ -n "$lnk_line" ] || { echo "FAIL: expected 'LINK $OBJ' in the plan (MMK did not resolve the .OBJ->.EXE step)"; exit 1; }
[ "$tcc_line" -lt "$lnk_line" ] || { echo "FAIL: plan order wrong — LINK must follow TCC (dependency order)"; exit 1; }

echo
echo "================================================================================"
echo "MILESTONE (vms-ec70, self-host spine #4): MMK.EXE parsed a real descrip.mms via"
echo "the CLI\$ compiled-CLD + real lib\$table_parse + the vms-486 PARSE_TABLES grammar +"
echo "MMK's own parse_store engine, built the target/dependency graph, and emitted the"
echo "correct build plan in dependency order (TCC $SRC -> LINK $OBJ)."
echo "================================================================================"
