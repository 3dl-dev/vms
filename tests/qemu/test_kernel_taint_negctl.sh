#!/bin/bash
# test_kernel_taint_negctl.sh - negative controls for the taint-clean acceptance
#                               gate (rd vms-566, epic vms-19e "owns-kernel").
#
# A gate nobody has tried to evade is an assertion about nothing. The gate
# (tests/qemu/test_kernel_taint.sh) claims the kernel is UNTAINTED by OVMX's
# modules: no O bit (out-of-tree), no E bit (unsigned). This file proves the gate
# is TWO-SIDED -- that it goes RED when the kernel WOULD be tainted -- so a
# regression that reverts the modules to out-of-tree or unsigned cannot pass it.
#
# Runs inside the ovmx-boot Docker image (needs the boot initramfs -- for the
# real shipped vms.ko -- and `modinfo` from kmod). It does NOT boot
# QEMU: it sources the gate's PURE helpers and exercises them directly, which is
# what makes it cheap enough to run on every PR.
#
# THREE proofs, mirroring the gate's three:
#
#   (1) THE NUMERIC ASSERTION IS TWO-SIDED. taint_mask_forbidden() -- the exact
#       function the gate applies to the real /proc/sys/kernel/tainted mask --
#       must return FORBIDDEN for the O mask (4096), the E mask (8192) and O+E
#       (12288), and "clean" for 0. This is the ground-source link: reverting the
#       modules to out-of-tree sets bit 12 in the mask PID 1 reads, and to
#       unsigned sets bit 13; both are exactly what this function reddens on. So
#       the durable assertion provably reddens on the two reverts the gate exists
#       to catch, without needing an expensive kernel rebuild.
#
#   (2) THE intree DISCRIMINATOR IS TWO-SIDED. Take the REAL shipped vms.ko, flip
#       its .modinfo "intree=Y" to "intree=N" (a same-length byte edit, so the
#       ELF stays valid), and assert ko_intree_ok() -- green on the real module
#       -- goes RED. modpost stamps intree=N on an OUT-OF-TREE build, so this is
#       exactly the shipped-artifact regression the static check must catch.
#
#   (3) THE signature DISCRIMINATOR IS TWO-SIDED. Take the REAL shipped vms.ko,
#       corrupt the appended "~Module signature appended~" magic (a same-length
#       byte edit), so modinfo no longer recognizes the signature -- exactly how
#       an UNSIGNED module presents -- and assert ko_signed_ok() goes RED.
#
# EVERY mutation is proven to have LANDED (the copy differs from the original)
# before a RED is trusted -- an anchor that stops matching would make the control
# a silent no-op that certifies evasions as caught (the trap
# test_runtime_target_negctl.sh documents at length). Broken fixture != caught.
#
# Usage:
#   docker run --rm -v $PWD/tests/qemu/test_kernel_taint.sh:/gate.sh:ro \
#       -v $PWD/tests/qemu/test_kernel_taint_negctl.sh:/negctl.sh:ro \
#       --entrypoint bash ovmx-boot /negctl.sh /gate.sh
#
# Exit code 0 = every control behaved (gate is two-sided), 1 = a control failed.

set -uo pipefail

GATE="${1:-$(dirname "$0")/test_kernel_taint.sh}"
INITRD="${OVMX_INITRD:-/boot/initramfs-ovmx.cpio.gz}"

passed=0
failed=0
status=0

pass() { echo "  PASS: $1"; passed=$((passed + 1)); }
bad()  { echo "  FAIL: $1"; failed=$((failed + 1)); status=1; }

# Source ONLY the pure helpers from the gate (it returns early without booting).
if [ ! -f "$GATE" ]; then
    echo "FATAL: gate script not found: $GATE"; exit 1
fi
# shellcheck disable=SC1090
OVMX_TAINT_GATE_SOURCE=1 . "$GATE"

if ! declare -F taint_mask_forbidden >/dev/null; then
    echo "FATAL: taint_mask_forbidden not defined -- the gate's sourceable seam"
    echo "       (OVMX_TAINT_GATE_SOURCE) did not export the pure helpers."
    exit 1
fi

echo "=== taint-clean gate negative controls (vms-566): the gate must be two-sided ==="

# ---------------------------------------------------------------------------
# (0) POSITIVE CONTROL. A clean mask (0) must be accepted. Without this, a
# taint_mask_forbidden() that reds on EVERYTHING would pass every control below
# and prove nothing.
# ---------------------------------------------------------------------------
if taint_mask_forbidden 0 >/dev/null; then
    bad "positive control: taint_mask_forbidden 0 reported FORBIDDEN (should be clean)"
else
    pass "positive control: a clean mask (0) is accepted"
fi

# ---------------------------------------------------------------------------
# (1) NUMERIC ASSERTION TWO-SIDEDNESS. O, E and O+E must ALL redden.
# ---------------------------------------------------------------------------
expect_forbidden() {
    local mask="$1" name="$2" out
    out=$(taint_mask_forbidden "$mask")
    if taint_mask_forbidden "$mask" >/dev/null; then
        pass "$name: mask $mask -> $out"
    else
        bad "$name: mask $mask was accepted as '$out' -- the gate is NOT two-sided here"
    fi
}
expect_forbidden "$TAINT_O_BIT"                     "O bit (out-of-tree revert)"
expect_forbidden "$TAINT_E_BIT"                     "E bit (unsigned revert)"
expect_forbidden "$(( TAINT_O_BIT | TAINT_E_BIT ))" "O+E bits (both reverts)"

# Guard: O and E must not be silently allowlisted. If TAINT_ALLOWED_BITS ever
# grows to cover 4096 or 8192, the numeric assertion is dead -- fail loudly.
if (( TAINT_ALLOWED_BITS & TAINT_O_BIT )) || (( TAINT_ALLOWED_BITS & TAINT_E_BIT )); then
    bad "TAINT_ALLOWED_BITS ($TAINT_ALLOWED_BITS) allowlists O or E -- forbidden by vms-566"
else
    pass "allowlist does not cover O or E"
fi

# ---------------------------------------------------------------------------
# Extract the REAL shipped modules for (2) and (3). If this fails, the modinfo
# controls FAIL LOUD as broken fixtures -- they never silently pass.
# ---------------------------------------------------------------------------
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM
have_modules=1
if ! gzip -dc "$INITRD" 2>/dev/null | ( cd "$WORK" && cpio -idm 2>/dev/null ); then
    have_modules=0
fi
VMS_KO=$(find "$WORK" -name vms.ko 2>/dev/null | head -1)
# vms-165 retired vmsfs.ko; vms.ko is the only shipped OVMX module, so both the
# intree and the signature discriminators below operate on it (separate copies,
# separate mutations).

# mutation_landed <orig> <mutated> -- 0 iff they differ (the edit really landed).
mutation_landed() { ! cmp -s "$1" "$2"; }

# ---------------------------------------------------------------------------
# (2) intree DISCRIMINATOR. Real vms.ko is intree=Y; a copy stamped intree=N
# (the out-of-tree condition) must redden ko_intree_ok.
# ---------------------------------------------------------------------------
if [ "$have_modules" = "1" ] && [ -n "$VMS_KO" ]; then
    # Sanity: the real module passes (else the mutation red proves nothing).
    if ko_intree_ok "$VMS_KO"; then
        pass "intree control baseline: real vms.ko is intree=Y (ko_intree_ok green)"
    else
        bad "intree control baseline: real vms.ko is NOT intree=Y -- fixture broken"
    fi
    MUT="$WORK/vms-oot.ko"
    # Same-length edit: 'intree=Y' -> 'intree=N' inside .modinfo (Y and N are one
    # byte each, so every ELF offset is preserved and the module stays valid).
    LC_ALL=C sed 's/intree=Y/intree=N/' "$VMS_KO" > "$MUT"
    if ! mutation_landed "$VMS_KO" "$MUT"; then
        bad "intree control: the 'intree=Y'->'intree=N' edit did not change vms.ko"
        echo "        (anchor no longer matches -- re-anchor; do NOT relax the gate)"
    elif ko_intree_ok "$MUT"; then
        bad "intree control: ko_intree_ok CERTIFIED an intree=N module -- not two-sided"
        echo "        modinfo -F intree => '$(modinfo -F intree "$MUT" 2>/dev/null)'"
    else
        pass "intree control: an out-of-tree (intree=N) module reddens ko_intree_ok"
    fi
else
    bad "intree control: could not extract vms.ko from $INITRD -- broken fixture"
fi

# ---------------------------------------------------------------------------
# (3) signature DISCRIMINATOR. Real vms.ko is signed; a copy whose appended
# signature magic is corrupted (the unsigned condition, as modinfo sees it) must
# redden ko_signed_ok. (vms-165: was vmsfs.ko before the vmsfs VFS driver was
# retired; vms.ko is the only shipped module now.)
# ---------------------------------------------------------------------------
if [ "$have_modules" = "1" ] && [ -n "$VMS_KO" ]; then
    if ko_signed_ok "$VMS_KO"; then
        pass "signature control baseline: real vms.ko is signed (ko_signed_ok green)"
    else
        bad "signature control baseline: real vms.ko is NOT signed -- fixture broken"
    fi
    MUT="$WORK/vms-unsigned.ko"
    # Same-length edit: corrupt the trailing '~Module signature appended~' magic
    # (flip the leading '~M' to '~m'). modinfo keys the appended PKCS#7 signature
    # off this exact magic at EOF; corrupting it makes modinfo see an UNSIGNED
    # module -- signer/sig_id empty -- exactly the unsigned regression's shape.
    LC_ALL=C sed 's/~Module signature appended~/~module signature appended~/' "$VMS_KO" > "$MUT"
    if ! mutation_landed "$VMS_KO" "$MUT"; then
        bad "signature control: the signature-magic edit did not change vms.ko"
        echo "        (the appended-signature magic was not found -- re-anchor)"
    elif ko_signed_ok "$MUT"; then
        bad "signature control: ko_signed_ok CERTIFIED an unsigned-looking module -- not two-sided"
        echo "        modinfo -F signer => '$(modinfo -F signer "$MUT" 2>/dev/null)'"
    else
        pass "signature control: an unsigned module (magic corrupted) reddens ko_signed_ok"
    fi
else
    bad "signature control: could not extract vms.ko from $INITRD -- broken fixture"
fi

echo ""
echo "=== taint-clean gate negative controls: $passed passed, $failed failed ==="
exit "$status"
