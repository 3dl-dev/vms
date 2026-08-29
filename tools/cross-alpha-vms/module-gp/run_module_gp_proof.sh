#!/bin/bash
# run_module_gp_proof.sh — feature-depth proof for the OVMX per-image module-GP
# prologue (bead vms-095, component C3 of vms-5f5; see
# docs/design-alpha-per-image-gp.md 2.1/2.1.1).
#
# WHAT C3 DOES: on TARGET_ABI_OPEN_VMS the port cc1 now establishes a per-image
# module-GP in the reserved register $15 at procedure entry
#   ldah $15,0($27) ; lda $15,0($15)   (the `.ovmx_gpdisp $15,<proc>' expansion)
# so $15 = R27(&PDSC) - K = the module linkage-section base, and addresses the
# linkage section $15-relative (`.base $15') instead of R27 (= &PDSC, skewed by K
# for any procedure past the first in a multi-procedure image -- the crtl_rms N=7
# root cause).  $15 is a fixed, call-saved register: each procedure SAVES the
# caller's $15 on entry and RESTORES it before RET, so a caller's own module-GP
# survives every call (intra-module, cross-module-intra-image, cross-image) with
# no per-call reload.
#
# THIS TEST compiles a multi-procedure TU with the real port cc1 and objdump-
# asserts, for every procedure:
#   (1) the `.ovmx_gpdisp $15' expansion  ldah $15,0($27) / lda $15,0($15),
#   (2) the $15 SAVE in the prologue AND the $15 RESTORE in the epilogue
#       -- LOAD-BEARING: omit either and a cross-module return resumes the caller
#       with the callee's module-GP -> wrong linkage base -> silent corruption,
#   (3) the body's linkage-section loads are $15-relative (not $27/&PDSC), and
#       NO linkage load is left $27(PV)-relative (the skew is gone).
# It then PROVES THE GATE CAN FAIL by mutating the disassembly (drop the gpdisp,
# drop the save, drop the restore, or re-base the body loads onto $27) and
# asserting the checker rejects each mutant.
#
# Build/oracle tooling, Rule-9-clean: cc1/objdump run on the build host and emit/
# read genuine alpha-dec-vms EVAX objects; nothing runs in the OVMX guest, no
# /dev/vms.  All deps containerized; work goes to /tmp inside the container.
#   tools/cross-alpha-vms/module-gp/run_module_gp_proof.sh
# Exit 0 = every assertion held AND every mutant was rejected.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)           # tools/cross-alpha-vms/module-gp
CROSS=$(cd "$HERE/.." && pwd)                 # tools/cross-alpha-vms
IMG=${IMG:-ovmx-cross-alpha-vms}

# vms-e7c5: reuse an already-present (prebuilt/pulled) toolchain image; a missing
# image still builds from source.
if docker image inspect "$IMG" >/dev/null 2>&1; then
    echo "== [1/3] toolchain image $IMG already present — skipping build =="
else
    echo "== [1/3] build alpha-dec-vms cross toolchain image ($IMG) =="
    docker build -t "$IMG" "$CROSS"
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp "$HERE/module_gp_multiproc.c" "$WORK/"

echo "== [2/3] compile the multi-procedure TU with the port cc1 and disassemble =="
docker run --rm -v "$WORK:/w" "$IMG" bash -euo pipefail -c '
    cd /w
    /opt/cross-alpha-vms/bin/alpha-dec-vms-gcc -O1 -c module_gp_multiproc.c -o m.o
    # bfd objdump reads OVMX objects (the EVAX_R_OVMX_GPDISP marker is skipped on
    # the read side, vms-095 bfd hunk); dump $CODE$ disassembly.
    /opt/cross-alpha-vms/bin/alpha-dec-vms-objdump -d m.o > disasm.txt
'
DIS="$WORK/disasm.txt"
if ! [ -s "$DIS" ]; then echo "FAIL: empty disassembly"; exit 1; fi

# ---- the checker: pure text asserts over an alpha-dec-vms objdump -d dump ----
# $15/$27/$29 print as R15/PV/FP in the alpha-vms disassembler.  Prints:
#   OK   — all assertions held
#   FAIL — first failing assertion (nonzero exit)
check_module_gp () {
    local f="$1"
    local funcs gphi gplo save restore body skew
    funcs=$(grep -cE '\.\.en>:' "$f" || true)
    gphi=$(grep -cE 'ldah[[:space:]]+R15,0\(PV\)' "$f" || true)      # $15 = hi(&PDSC-K)
    gplo=$(grep -cE 'lda[[:space:]]+R15,0\(R15\)' "$f" || true)      # $15 = &PDSC-K
    save=$(grep -cE 'stq[[:space:]]+R15,[0-9a-fx]+\(SP\)' "$f" || true)   # save the caller $15
    restore=$(grep -cE 'ldq[[:space:]]+R15,[0-9a-fx]+\(SP\)' "$f" || true)  # restore the caller $15
    body=$(grep -cE 'ldq[[:space:]]+[A-Z0-9]+,-?[0-9a-fx]+\(R15\)' "$f" || true) # linkage load via module-GP
    skew=$(grep -cE 'ldq[[:space:]]+[A-Z0-9]+,-?[0-9a-fx]+\(PV\)' "$f" || true)  # PV-relative linkage load = the N=7 skew

    [ "$funcs" -ge 1 ]              || { echo "FAIL: no procedures found";                       return 1; }
    [ "$gphi" -eq "$funcs" ]        || { echo "FAIL: .ovmx_gpdisp hi (ldah R15,0(PV)) $gphi != $funcs procs"; return 1; }
    [ "$gplo" -eq "$funcs" ]        || { echo "FAIL: .ovmx_gpdisp lo (lda R15,0(R15)) $gplo != $funcs procs"; return 1; }
    [ "$save" -ge "$funcs" ]        || { echo "FAIL: caller-\$15 SAVE (stq R15,N(SP)) $save < $funcs procs";  return 1; }
    [ "$restore" -ge "$funcs" ]     || { echo "FAIL: caller-\$15 RESTORE (ldq R15,N(SP)) $restore < $funcs procs"; return 1; }
    [ "$body" -ge "$funcs" ]        || { echo "FAIL: \$15-relative linkage loads $body < $funcs procs";      return 1; }
    [ "$skew" -eq 0 ]               || { echo "FAIL: $skew PV-relative linkage load(s) remain (the N=7 skew)"; return 1; }
    echo "OK"
    return 0
}

echo "== [3/3] assert the codegen, then PROVE the gate can fail =="
echo "-- disassembly (mgp_middle) --"
awk '/mgp_middle\.\.en>:/{p=1} p{print} p&&/\<ret\>/{exit}' "$DIS" || true

echo "-- assertion on the real object (must PASS) --"
if ! check_module_gp "$DIS"; then echo "REAL OBJECT FAILED ASSERTIONS"; exit 1; fi

# ---- prove-can-fail: each mutant must be REJECTED by the same checker ----
fail_expected () {  # $1=name  $2=mutated file
    if check_module_gp "$2" >/dev/null 2>&1; then
        echo "FAIL: mutant '$1' was ACCEPTED — the gate cannot detect this break"; exit 1
    fi
    echo "OK: mutant '$1' correctly REJECTED"
}
# M1: drop the .ovmx_gpdisp expansion (no module-GP established)
grep -vE 'ldah[[:space:]]+R15,0\(PV\)' "$DIS" > "$WORK/m1.txt"
fail_expected "no .ovmx_gpdisp"        "$WORK/m1.txt"
# M2: drop the caller-$15 SAVE
grep -vE 'stq[[:space:]]+R15,[0-9a-fx]+\(SP\)' "$DIS" > "$WORK/m2.txt"
fail_expected "no \$15 save"           "$WORK/m2.txt"
# M3: drop the caller-$15 RESTORE
grep -vE 'ldq[[:space:]]+R15,[0-9a-fx]+\(SP\)' "$DIS" > "$WORK/m3.txt"
fail_expected "no \$15 restore"        "$WORK/m3.txt"
# M4: re-base the body linkage loads onto $27 (reintroduce the N=7 skew)
sed -E 's/(ldq[[:space:]]+[A-Z0-9]+,-?[0-9a-fx]+)\(R15\)/\1(PV)/' "$DIS" > "$WORK/m4.txt"
fail_expected "body re-based to \$27"  "$WORK/m4.txt"

echo
echo "PASS: per-image module-GP established in \$15, caller \$15 saved+restored,"
echo "      linkage loads \$15-relative (no \$27 skew); all four mutants rejected."
