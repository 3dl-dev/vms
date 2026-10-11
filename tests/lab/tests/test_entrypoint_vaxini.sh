#!/bin/bash
# test_entrypoint_vaxini.sh - the vax.ini every lab node boots from carries the
# SIMH settings that keep a node alive (rd vms-549c).
#
# Hermetic: renders the vax.ini here-document out of tests/lab/entrypoint.sh
# with stand-in paths (no pod, no SIMH) and asserts:
#   - 'set idle=vms'        (an idle VMS must not peg a core)
#   - 'set clock nocatchup' (timer catch-up overflowed the calibration and hung
#                            nodes: Current Insts Per Tick went negative)
#   - both precede the final 'b', so they take effect before the boot.
# Negative control: the same check on a copy with the nocatchup line removed
# must FAIL, so this gate can go red.
set -u
SELF_DIR=$(cd "$(dirname "$0")" && pwd)
EP="${1:-$SELF_DIR/../entrypoint.sh}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

render() {   # render <entrypoint> -> stdout: the vax.ini a node would get
	local body
	body=$(sed -n '/cat > "${nd}\/vax.ini" <<EOF/,/^EOF$/p' "$1" | sed '1d;$d')
	[ -n "$body" ] || return 1
	nd=/n LAB_DIR=/l LAB_ROOT=/r eval "cat <<EOF
$body
EOF"
}

check() {    # check <rendered-file> -> 0 iff every rule holds
	local f=$1 b n i
	b=$(grep -n '^b$' "$f" | tail -1 | cut -d: -f1)
	[ -n "$b" ] || { echo "  no final 'b' line"; return 1; }
	for want in 'set idle=vms' 'set clock nocatchup'; do
		n=$(grep -n -x -F "$want" "$f" | head -1 | cut -d: -f1)
		[ -n "$n" ] || { echo "  missing: $want"; return 1; }
		[ "$n" -lt "$b" ] || { echo "  '$want' comes after the boot"; return 1; }
	done
	return 0
}

rc=0
render "$EP" > "$TMP/vax.ini" || { echo "FAIL: could not render vax.ini from $EP"; exit 1; }
if check "$TMP/vax.ini"; then
	echo "ok: the generated vax.ini sets idle=vms and clock nocatchup before booting"
else
	echo "FAIL: the generated vax.ini is missing a load-bearing SIMH setting"; rc=1
fi

grep -v -x -F 'set clock nocatchup' "$EP" > "$TMP/ep-neg.sh"
render "$TMP/ep-neg.sh" > "$TMP/neg.ini"
if check "$TMP/neg.ini" >/dev/null; then
	echo "FAIL: negative control -- a vax.ini WITHOUT nocatchup passed"; rc=1
else
	echo "ok: negative control -- removing nocatchup reddens the check"
fi
exit $rc
