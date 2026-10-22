#!/bin/sh
# tests/ods2/test_ods2_relabel.sh
#
# rd vms-3ea: `vmsfs_master relabel` gives an existing ODS-2 volume a new label
# offline -- the on-disk effect of SET VOLUME/LABEL -- through the codec's own
# writers (ods2_home_set_label, ods2_security_set_label, ods2_bdev_relabel).
#
# WHY IT MATTERS. The volume label is the Files-11 cluster lock namespace: the
# executive names a volume's locks "F11B$<kind><label>". Every OVMX node used to
# boot a copy of one kit mastered OVMXSYS, so two members' DIFFERENT system
# disks shared one clusterwide resource. Provisioning now relabels each node's
# copy <SCSNODE>SYS (the form the real VAX uses: SCSNODE VAXC -> VAXCSYS).
#
# WHAT THIS PROVES, with the real binary:
#   1. A relabelled volume is BYTE-IDENTICAL to one mastered with the new label
#      from the start (same tree, same SOURCE_DATE_EPOCH). So every place the
#      label lives -- both home blocks, both checksums, the SECURITY.SYS profile
#      -- is rewritten, and nothing else is touched. Leaving any one of them
#      stale breaks the cmp.
#   2. A volume a REAL VAX wrote (tests/ods2/real_vax_ods2.dsk, label OVMXTEST)
#      relabels: list reads the new label back, both home blocks carry it, and
#      the only blocks that changed are the two home blocks and one more (the
#      SECURITY.SYS profile).
#   3. REFUSALS WRITE NOTHING: an invalid label, a volume whose alternate home
#      block names a different label, and a profile shape this codec does not
#      know (the real Alpha ODS-5 volume) each fail and leave the image
#      byte-for-byte as it was.
#
# VMSFS_MASTER is injected by CMake; ODS2_REAL_FIXTURE / ODS5_REAL_FIXTURE are
# the two real-system volumes.

set -eu

MASTER="${VMSFS_MASTER:?VMSFS_MASTER (path to vmsfs_master) must be set}"
REAL="${ODS2_REAL_FIXTURE:?ODS2_REAL_FIXTURE must name tests/ods2/real_vax_ods2.dsk}"
REAL5="${ODS5_REAL_FIXTURE:?ODS5_REAL_FIXTURE must name the real Alpha ODS-5 volume}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ods2relabel.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT INT TERM

fail() { echo "FAIL: $*" >&2; exit 1; }
label_at() {  # image lbn -> the 12-byte label field of the block, trimmed
    dd if="$1" bs=1 skip=$(( $2 * 512 + 472 )) count=12 2>/dev/null | tr -d ' '
}

# ---- 1. relabel == master with the new label, byte for byte ----
TREE="$WORK/tree"
mkdir -p "$TREE/SYS0/SYSCOMMON/SYSEXE"
printf '$ WRITE SYS$OUTPUT "hello"\n' > "$TREE/SYS0/SYSCOMMON/SYSEXE/LOGIN.COM"
head -c 70000 /dev/urandom > "$TREE/SYS0/SYSCOMMON/SYSEXE/DCL.EXE"
SOURCE_DATE_EPOCH=1700000000 "$MASTER" --ods2 master "$WORK/a.img" OVMXSYS "$TREE" 8 >/dev/null \
    || fail "master OVMXSYS"
SOURCE_DATE_EPOCH=1700000000 "$MASTER" --ods2 master "$WORK/b.img" OVMXASYS "$TREE" 8 >/dev/null \
    || fail "master OVMXASYS"
cmp -s "$WORK/a.img" "$WORK/b.img" && fail "two labels mastered identical images (the comparison has no teeth)"
"$MASTER" --ods2 relabel "$WORK/a.img" OVMXASYS > "$WORK/r1.out" 2>&1 \
    || { cat "$WORK/r1.out" >&2; fail "relabel of a mastered volume exited non-zero"; }
grep -q 'MASTER-I-RELABEL, .*volume OVMXSYS is now OVMXASYS' "$WORK/r1.out" \
    || { cat "$WORK/r1.out" >&2; fail "relabel did not report old -> new"; }
cmp "$WORK/a.img" "$WORK/b.img" >/dev/null \
    || fail "relabelled OVMXSYS -> OVMXASYS is not byte-identical to a volume mastered OVMXASYS"
echo "PASS: relabel OVMXSYS -> OVMXASYS is byte-identical to mastering OVMXASYS"

# ---- 2. a volume a real VAX wrote ----
cp "$REAL" "$WORK/real.img"
[ "$(label_at "$WORK/real.img" 1)" = OVMXTEST ] || fail "fixture's own label is not OVMXTEST"
"$MASTER" --ods2 relabel "$WORK/real.img" VAXRELAB > "$WORK/r2.out" 2>&1 \
    || { cat "$WORK/r2.out" >&2; fail "relabel of the real-VAX volume exited non-zero"; }
"$MASTER" --ods2 list "$WORK/real.img" > "$WORK/l2.out" 2>&1 || fail "list after relabel"
grep -q 'MASTER-I-VOLUME, VAXRELAB ' "$WORK/l2.out" || { cat "$WORK/l2.out" >&2; fail "list does not read VAXRELAB back"; }
[ "$(label_at "$WORK/real.img" 1)" = VAXRELAB ] || fail "primary home block (LBN 1) not relabelled"
[ "$(label_at "$WORK/real.img" 2)" = VAXRELAB ] || fail "alternate home block (LBN 2) not relabelled"
blocks=$(cmp -l "$REAL" "$WORK/real.img" | awk '{ print int(($1 - 1) / 512) }' | sort -un | tr '\n' ' ')
nblk=$(printf '%s\n' $blocks | wc -l)
case " $blocks" in *" 1 "*) ;; *) fail "LBN 1 did not change (changed: $blocks)";; esac
case " $blocks" in *" 2 "*) ;; *) fail "LBN 2 did not change (changed: $blocks)";; esac
[ "$nblk" -eq 3 ] || fail "expected exactly the home pair + the SECURITY.SYS profile to change, got blocks: $blocks"
for f in 000000.DIR BACKUP.SYS; do
    grep -q "$f" "$WORK/l2.out" || fail "relabelled volume no longer lists $f"
done
echo "PASS: a real VAX volume relabels (home pair + SECURITY.SYS, blocks $blocks)"

# ---- 3. refusals write nothing ----
cp "$REAL" "$WORK/bad.img"
for bad in '' 'bad.label' 'lower' 'THIRTEENCHARS'; do
    if "$MASTER" --ods2 relabel "$WORK/bad.img" "$bad" > "$WORK/r3.out" 2>&1; then
        fail "relabel accepted the invalid label '$bad'"
    fi
done
cmp -s "$REAL" "$WORK/bad.img" || fail "a refused invalid label still changed the image"

# The alternate home block names another label: the volume disagrees with
# itself, so which label it "has" is not knowable -- refuse, write nothing.
cp "$REAL" "$WORK/split.img"
"$MASTER" --ods2 relabel "$WORK/split.img" OTHER >/dev/null 2>&1 || fail "setup relabel"
dd if="$REAL" of="$WORK/split.img" bs=512 skip=1 seek=1 count=1 conv=notrunc 2>/dev/null
cp "$WORK/split.img" "$WORK/split.before"
if "$MASTER" --ods2 relabel "$WORK/split.img" NEWER > "$WORK/r4.out" 2>&1; then
    fail "relabel accepted a volume whose two home blocks name different labels"
fi
grep -q 'MASTER-F-RELABEL, .*nothing written' "$WORK/r4.out" || { cat "$WORK/r4.out" >&2; fail "no RELABEL refusal"; }
cmp -s "$WORK/split.before" "$WORK/split.img" || fail "a refused split-label volume was written"

# A SECURITY.SYS profile shape the codec did not write and does not know (the
# real Alpha ODS-5 volume): refused, untouched.
cp "$REAL5" "$WORK/o5.img"
if "$MASTER" --ods2 relabel "$WORK/o5.img" ODS5NEW > "$WORK/r5.out" 2>&1; then
    fail "relabel rewrote a SECURITY.SYS profile it does not know"
fi
cmp -s "$REAL5" "$WORK/o5.img" || fail "a refused ODS-5 relabel still changed the image"
echo "PASS: invalid labels, a split-label volume and an unknown profile are refused with nothing written"

echo "ALL PASS: vmsfs_master relabel (rd vms-3ea)"
