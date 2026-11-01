#!/bin/bash
# test_fire_cluster_group.sh - a lab fire REFUSES an initramfs that would boot
# on no cluster's wire (rd vms-daa).
#
# WHAT IS BEING PROVEN. The cluster group number is what the LAVC HELLO
# multicast address is built from, so a node booted with group 0 -- which is
# what an ORDINARY image build produces, since it ships an empty /etc/ovmx --
# transmits HELLOs to AB-00-04-01-00-00, receives none, and NEVER JOINS. It is
# silent: no failure, no console line, just a capture with no join in it, and
# that cost a whole lab run (measured on lab-2 vaxlab-4, 2026-09-20: 225 frames
# to AB-00-04-01-00-00 while the real V7.3 cluster used AB-00-04-01-01-01).
# tests/lab/tools/check_cluster_group.sh is the precondition ovmx_fire.sh now
# applies, and this is its gate.
#
# HERMETIC: no pod, no kubectl, no QEMU, no VAX. It builds four tiny initramfs
# images with the project's OWN record writer (tools/cluster/
# mk_cluster_authorize.c, through tests/lab/tools/stage_cluster_group.sh where
# that applies) and runs the check against each:
#
#   no record at all        REFUSED   (an ordinary build: group 0)
#   a record saying 0       REFUSED   (explicitly the void)
#   an unreadable record    REFUSED   (the executive reads nothing there either)
#   a record saying 1       ACCEPTED, and the group is printed
#   group 1, requiring 2    REFUSED   (another group is another cluster's wire)
#
# NEGATIVE CONTROL (Rule 7): the same five cases against a COPY of the checker
# with its group-0 test removed must stop refusing -- so this gate is proven
# able to go red, and a checker that accepted everything could not pass here.
set -u

SELF_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SELF_DIR/../../.." && pwd)
CHECK="$ROOT/tests/lab/tools/check_cluster_group.sh"
STAGE="$ROOT/tests/lab/tools/stage_cluster_group.sh"
MKSRC="$ROOT/tools/cluster/mk_cluster_authorize.c"
AUTHHDR="$ROOT/src/libvms/include"
CC="${CC:-cc}"

TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT
rc=0
pass() { echo "  ok   $*"; }
fail() { echo "  FAIL $*"; rc=1; }

for f in "$CHECK" "$STAGE" "$MKSRC"; do
	[ -f "$f" ] || { echo "FATAL: missing $f"; exit 2; }
done
command -v cpio >/dev/null 2>&1 || { echo "FATAL: cpio is not on this host"; exit 2; }
command -v "$CC" >/dev/null 2>&1 || { echo "FATAL: no C compiler ($CC)"; exit 2; }

# ---------------------------------------------------------------- fixtures
"$CC" -I"$AUTHHDR" "$MKSRC" -o "$TMP/mk" >/dev/null 2>&1 \
	|| { echo "FATAL: could not build mk_cluster_authorize"; exit 2; }

pack() {   # pack <root-dir> <out.cpio.gz>
	( cd "$1" && find . | cpio -o -H newc --quiet ) | gzip -9 > "$2"
}

mkdir -p "$TMP/norec/etc/ovmx"
pack "$TMP/norec" "$TMP/norec.cpio.gz"

mkdir -p "$TMP/g0/etc/ovmx"
"$TMP/mk" "$TMP/g0/etc/ovmx/cluster_authorize.dat" 0 >/dev/null 2>&1
pack "$TMP/g0" "$TMP/g0.cpio.gz"

mkdir -p "$TMP/bad/etc/ovmx"
printf 'not a CLUSTER_AUTHORIZE record' > "$TMP/bad/etc/ovmx/cluster_authorize.dat"
pack "$TMP/bad" "$TMP/bad.cpio.gz"

# The accepted one is staged by the project's own tool, so the test and the lab
# agree on how a real group gets into an image.
"$STAGE" "$TMP/norec.cpio.gz" "$TMP/g1.cpio.gz" 1 >/dev/null 2>&1 \
	|| { echo "FATAL: stage_cluster_group.sh could not stage group 1"; exit 2; }

# ---------------------------------------------------------------- the gate
run() {    # run <checker> <image> [required] -> exit code, output in $OUT
	OUT=$( "$1" "$2" ${3:+"$3"} 2>&1 ); return $?
}

echo "--- the precondition refuses what never joins ---"
for case in norec:0 g0:0 bad:0; do
	img=${case%%:*}
	if run "$CHECK" "$TMP/$img.cpio.gz"; then
		fail "$img.cpio.gz was ACCEPTED -- a node that joins nothing"
	else
		pass "$img.cpio.gz is refused: ${OUT#*REFUSED -- }"
	fi
done

if run "$CHECK" "$TMP/g1.cpio.gz"; then
	case "$OUT" in
	*"cluster group 1"*) pass "a group-1 initramfs is accepted, and the group is named" ;;
	*) fail "a group-1 initramfs was accepted but the group was not named: $OUT" ;;
	esac
else
	fail "a group-1 initramfs was REFUSED: $OUT"
fi

if run "$CHECK" "$TMP/g1.cpio.gz" 1; then
	pass "...and it satisfies a fire that REQUIRES group 1"
else
	fail "a group-1 initramfs did not satisfy a required group of 1: $OUT"
fi

if run "$CHECK" "$TMP/g1.cpio.gz" 2; then
	fail "group 1 satisfied a fire that requires group 2"
else
	pass "a fire that requires group 2 refuses it: another group is another wire"
fi

# ---------------------------------------------------------------- the teeth
echo "--- negative control: a checker without the group-0 test stops refusing ---"
sed 's|^\[ "\$GOT" -ne 0 \] |[ 1 -eq 1 ] |' "$CHECK" > "$TMP/blind.sh"
chmod +x "$TMP/blind.sh"
export OVMX_ROOT="$ROOT"      # the copy runs from a temp dir; same tree
if cmp -s "$CHECK" "$TMP/blind.sh"; then
	fail "the negative control changed nothing -- it cannot prove anything"
elif run "$TMP/blind.sh" "$TMP/g0.cpio.gz"; then
	pass "the blinded checker ACCEPTS the group-0 image: the test above has teeth"
else
	fail "the blinded checker still refused group 0 -- this gate is not testing what it says"
fi

# ...and that the fire script really applies it, not merely ships it.
echo "--- ovmx_fire.sh calls the precondition ---"
if grep -q 'check_cluster_group.sh' "$ROOT/tests/lab/tools/ovmx_fire.sh"; then
	pass "ovmx_fire.sh runs check_cluster_group.sh"
else
	fail "ovmx_fire.sh does not run check_cluster_group.sh"
fi
if grep -q 'initramfs-ovmx-slim.cpio.gz' "$ROOT/tests/lab/tools/ovmx_evnode.sh" &&
   grep -q 'initramfs-ovmx-slim.cpio.gz' "$ROOT/tests/lab/tools/ovmx_fire.sh"; then
	pass "...on the same initramfs ovmx_evnode.sh passes to -initrd"
else
	fail "the checked initramfs is not the one that boots"
fi

echo
[ $rc -eq 0 ] && echo "PASS: a lab fire cannot boot a node onto no cluster's wire" \
	|| echo "FAIL: see the FAIL lines above"
exit $rc
