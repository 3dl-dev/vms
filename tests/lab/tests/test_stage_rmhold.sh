#!/bin/bash
# test_stage_rmhold.sh -- hermetic check of tests/lab/tools/stage_rmhold.sh (no
# pod): the dry run stages the committed RMHOLD.MAR and, per VAX, runs MACRO,
# LINK, COPY to SYS$COMMON:[SYSEXE], defines RMH and verifies, in that order.
# Negative controls: a non-VAX node and a missing RMHOLD.MAR must be refused.
set -eu
T=$(cd "$(dirname "$0")/../tools" && pwd)
S="$T/stage_rmhold.sh"
fail() { echo "FAIL: $*" >&2; exit 1; }

bash -n "$S" || fail "bash -n"
grep -q '^ *\.TITLE RMHOLD' "$T/../ci6/RMHOLD.MAR" || fail "RMHOLD.MAR not the RMHOLD source"
python3 -c "import ast,sys; ast.parse(open(sys.argv[1]).read())" "$T/vput.py" || fail "vput.py"

out=$("$S" --dry-run podx vax1 vax3)
ln=0
for w in 'vput.py 10.2.1.1' 'MACRO RMHOLD' 'LINK RMHOLD' \
         'COPY RMHOLD.EXE SYS$COMMON:[SYSEXE]RMHOLD.EXE' 'RMH :== $SYS$SYSTEM:RMHOLD' \
         'DIRECTORY/SIZE SYS$SYSTEM:RMHOLD.EXE' 'staged RMHOLD on vax1' \
         'vput.py 10.2.1.3' 'MACRO RMHOLD' 'staged RMHOLD on vax3'; do
	n=$(echo "$out" | tail -n +$((ln + 1)) | grep -nF -- "$w" | head -1 | cut -d: -f1)
	[ -n "$n" ] || fail "missing or out of order after line $ln: $w"
	ln=$((ln + n))
done
echo "$out" | grep -q 'cp .*ci6/RMHOLD.MAR podx:/lab/run-rmhold/RMHOLD.MAR' || fail "MAR not copied into pod"

# negative controls
if "$S" --dry-run podx ovmxe >/dev/null 2>&1; then fail "accepted a non-VAX node"; fi
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/tools" "$tmp/ci6"; cp "$S" "$tmp/tools/"
if "$tmp/tools/stage_rmhold.sh" --dry-run podx >/dev/null 2>&1; then fail "ran without RMHOLD.MAR"; fi
echo "PASS: stage_rmhold.sh"
