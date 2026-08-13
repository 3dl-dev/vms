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
#   5. APPEND a new entry at the end (the one legal change)  -> gate GREEN
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

# 5. append a new entry at the end (the ONE legal change).
seed "$TMP/t5"
cat > "$TMP/t5/src/vmslink/demo_shr.vec" <<'EOF'
alpha=PROCEDURE
beta=PROCEDURE
gamma=DATA
delta=PROCEDURE
EOF
expect "append at end" GREEN "$(verdict "$TMP/t5")"

[ "$fail" -eq 0 ] && echo "PASS: freeze gate reddens on reorder/drop/dup/inline-copy and greens on append"
exit "$fail"
