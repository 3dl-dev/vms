#!/bin/sh
#
# test_symvec_freeze_negctl.sh — negative control for test_symvec_freeze.sh
# (bead vms-bd1). A freeze gate is worthless unless it actually reddens on the
# thing it forbids. This drives the REAL gate against a disposable source tree
# and requires:
#   1. REORDER a frozen entry  -> gate RED
#   2. DROP a frozen entry     -> gate RED
#   3. DUPLICATE a name        -> gate RED
#   4. re-grow an inline vms_kif_* vector copy in a harness -> gate RED
#   5. APPEND a new entry to the released list outside a cut -> gate RED
#   6. ADD a fragment <manifest>.d/<NAME> (the legal change)  -> gate GREEN
#   7. a fragment naming a released universal                -> gate RED
#   8. a fragment with a bad class                           -> gate RED
#   9. a fragment directory for a VMS-ordinal vector         -> gate RED
#  10. a release cut: the same entry appended to .vec AND .frozen -> GREEN
#
# Each case is minimal and isolated; only the innocent case (5) may pass.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
GATE="$HERE/test_symvec_freeze.sh"
[ -f "$GATE" ] || { echo "FAIL: gate not found: $GATE"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

# Build a minimal, valid source tree the gate can chew on.
seed() {
    root=$1
    rm -rf "$root"
    mkdir -p "$root/src/vmslink" "$root/src/imgact/test"
    cp "$HERE/../../src/vmslink/symvec_emit.sh" "$root/src/vmslink/symvec_emit.sh"
    cat > "$root/src/vmslink/demo_shr.vec" <<'EOF'
# demo manifest
alpha=PROCEDURE
beta=PROCEDURE
gamma=DATA
EOF
    cat > "$root/src/vmslink/demo_shr.vec.frozen" <<'EOF'
alpha=PROCEDURE
beta=PROCEDURE
gamma=DATA
EOF
}

# Run the gate; echo PASS/FAIL outcome as a token.
verdict() {
    if sh "$GATE" "$1" >"$TMP/out" 2>&1; then echo GREEN; else echo RED; fi
}

expect() {
    label=$1; want=$2; got=$3
    if [ "$got" = "$want" ]; then
        echo "OK   $label -> $got (expected $want)"
    else
        echo "FAIL $label -> $got (expected $want)"; sed 's/^/      | /' "$TMP/out"; fail=1
    fi
}

# 0. sanity: the pristine seed is GREEN.
seed "$TMP/t0"; expect "pristine seed" GREEN "$(verdict "$TMP/t0")"

# 1. reorder a frozen entry.
seed "$TMP/t1"
cat > "$TMP/t1/src/vmslink/demo_shr.vec" <<'EOF'
beta=PROCEDURE
alpha=PROCEDURE
gamma=DATA
EOF
expect "reorder frozen entry" RED "$(verdict "$TMP/t1")"

# 2. drop a frozen entry.
seed "$TMP/t2"
cat > "$TMP/t2/src/vmslink/demo_shr.vec" <<'EOF'
alpha=PROCEDURE
gamma=DATA
EOF
expect "drop frozen entry" RED "$(verdict "$TMP/t2")"

# 3. duplicate a name.
seed "$TMP/t3"
cat > "$TMP/t3/src/vmslink/demo_shr.vec" <<'EOF'
alpha=PROCEDURE
beta=PROCEDURE
gamma=DATA
beta=PROCEDURE
EOF
expect "duplicate name" RED "$(verdict "$TMP/t3")"

# 4. an inline vms_kif_* vector copy re-grows in a harness script.
seed "$TMP/t4"
printf '%s\n' '  --symbol-vector "vms_strlen=PROCEDURE,vms_kif_open=PROCEDURE,vms_kif_enq=PROCEDURE,vms_kif_deq=PROCEDURE"' \
    > "$TMP/t4/src/imgact/test/run_bogus_native.sh"
expect "inline vms_kif vector re-grew" RED "$(verdict "$TMP/t4")"

# 5. append a new entry to the released list outside a cut.
seed "$TMP/t5"
cat > "$TMP/t5/src/vmslink/demo_shr.vec" <<'EOF'
alpha=PROCEDURE
beta=PROCEDURE
gamma=DATA
delta=PROCEDURE
EOF
expect "append to the released list outside a cut" RED "$(verdict "$TMP/t5")"

# 6. add a fragment (the legal change between releases).
seed "$TMP/t6"
mkdir -p "$TMP/t6/src/vmslink/demo_shr.d"
echo PROCEDURE > "$TMP/t6/src/vmslink/demo_shr.d/delta"
expect "add a fragment" GREEN "$(verdict "$TMP/t6")"

# 7. a fragment naming a released universal.
seed "$TMP/t7"
mkdir -p "$TMP/t7/src/vmslink/demo_shr.d"
echo PROCEDURE > "$TMP/t7/src/vmslink/demo_shr.d/beta"
expect "fragment duplicates a released name" RED "$(verdict "$TMP/t7")"

# 8. a fragment with a bad class.
seed "$TMP/t8"
mkdir -p "$TMP/t8/src/vmslink/demo_shr.d"
echo FUNCTION > "$TMP/t8/src/vmslink/demo_shr.d/delta"
expect "fragment with a bad class" RED "$(verdict "$TMP/t8")"

# 9. a fragment directory for a VMS-ordinal vector.
seed "$TMP/t9"
mkdir -p "$TMP/t9/src/vmslink/vms_vectors/LIBRTL.d"
echo PROCEDURE > "$TMP/t9/src/vmslink/vms_vectors/LIBRTL.d/LIB\$NEW"
expect "fragments for a VMS-ordinal vector" RED "$(verdict "$TMP/t9")"

# 10. a release cut appends to the released list AND its golden.
seed "$TMP/t10"
printf 'delta=PROCEDURE\n' >> "$TMP/t10/src/vmslink/demo_shr.vec"
printf 'delta=PROCEDURE\n' >> "$TMP/t10/src/vmslink/demo_shr.vec.frozen"
expect "release cut (vec + golden)" GREEN "$(verdict "$TMP/t10")"

[ "$fail" -eq 0 ] && echo "PASS: freeze gate reddens on reorder/drop/dup/inline-copy/out-of-cut append/bad fragments and greens on fragments and cuts"
exit "$fail"
