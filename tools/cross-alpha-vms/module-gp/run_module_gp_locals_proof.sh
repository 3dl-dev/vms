#!/bin/bash
# run_module_gp_locals_proof.sh — feature-depth proof that the OVMX linker
# resolves the per-image module-GP K for LOCAL/static and weak-overridden
# procedures, not just global universals (bead vms-095, component C3 of vms-5f5;
# see docs/design-alpha-per-image-gp.md §2.2.1).
#
# THE BUG this guards (found linking the real alpha DECC$SHR, vms-864): the port
# cc1 correctly emits `.ovmx_gpdisp $15,<proc>' for EVERY procedure that touches
# the linkage section -- including STATIC/local ones (musl's io_thread_func) and
# a WEAK def a strong def overrides. C1's first K-lookup was keyed on GLOBAL (EGSD)
# procedure names, so a LOCAL proc (absent from EGSD) hard-errored
# %LINK-F-GPDISPUNDEF even though it IS a defined procedure with a real PDSC and K.
# The all-globals LIBOTS$ fixture (every proc an exported OTS$ universal) could not
# exercise this.
#
# THE FIX resolves K from the PDSC PLACEMENT the reloc carries (pdsc_offset +
# in[ii].$LINK$ base) + the strong-over-weak redirect (evax_wredir), covering
# local, global, AND weak-overridden uniformly -- no global-name table.
#
# THIS TEST compiles a two-object fixture that a global-name table could not link:
#   mgp_locals_a.c: static_helper (STATIC/local), shared_proc (WEAK), leaf_extern
#                   (PT_NULL-extern leaf) -- each references the linkage section.
#   mgp_locals_b.c: the STRONG shared_proc that overrides A's weak one.
# It links a shareable and asserts, from LINK.EXE's %LINK-I-GPDISP diagnostics:
#   (1) the link SUCCEEDS with ZERO %LINK-F-GPDISPUNDEF (pre-fix: hard error on the
#       local/static proc);
#   (2) static_helper (a LOCAL proc) resolved to a NONZERO K (a pre-fix stub that
#       read the old always-zero operand field would give K=0);
#   (3) leaf_extern (PT_NULL-extern leaf) resolved;
#   (4) shared_proc's WEAK-def site shows weakover=1 with raw_k (the weak PDSC's K)
#       != K, and its resolved K equals the STRONG def-site's K -- i.e. the weak
#       was redirected to the surviving strong def, never bound to the discarded
#       weak one.
# Then it PROVES THE GATE CAN FAIL by mutating the diagnostics (drop the local
# resolution, zero the local K, or bind the weak site to its own weak K) and
# asserting the checker rejects each mutant.
#
# Build/oracle tooling, Rule-9-clean: cc1 + host LINK.EXE run on the build host and
# emit/read genuine alpha-dec-vms EVAX objects; nothing runs in the OVMX guest.
#   tools/cross-alpha-vms/module-gp/run_module_gp_locals_proof.sh
# Exit 0 = every assertion held AND every mutant was rejected.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)           # tools/cross-alpha-vms/module-gp
CROSS=$(cd "$HERE/.." && pwd)                 # tools/cross-alpha-vms
REPO=$(cd "$CROSS/../.." && pwd)
IMG=${IMG:-ovmx-cross-alpha-vms}

if docker image inspect "$IMG" >/dev/null 2>&1; then
    echo "== [1/3] toolchain image $IMG already present — skipping build =="
else
    echo "== [1/3] build alpha-dec-vms cross toolchain image ($IMG) =="
    docker build -t "$IMG" "$CROSS"
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "== [2/3] compile the local/weak fixture, build LINK.EXE, link a shareable =="
docker run --rm -v "$REPO:/wt:ro" -v "$WORK:/out" "$IMG" bash -euo pipefail -c '
    export PATH=/opt/cross-alpha-vms/bin:$PATH
    cd /tmp
    alpha-dec-vms-gcc -O1 -c /wt/tools/cross-alpha-vms/module-gp/mgp_locals_a.c -o a.o
    alpha-dec-vms-gcc -O1 -c /wt/tools/cross-alpha-vms/module-gp/mgp_locals_b.c -o b.o
    gcc -std=gnu11 -O2 -I/wt/src/vmslink/include -o LINK.EXE /wt/src/vmslink/link.c
    set +e
    OVMX_LINK_DUMP_GP=1 ./LINK.EXE --shareable \
        --symbol-vector "entry_a=PROCEDURE,shared_proc=PROCEDURE,leaf_extern=PROCEDURE,take_static=PROCEDURE" \
        --gsmatch LEQUAL,1,0 -o "/out/MGP\$SHR.EXE" a.o b.o > /out/link.log 2>&1
    echo "LINK_EXIT=$?" >> /out/link.log
'
LOG="$WORK/link.log"
[ -s "$LOG" ] || { echo "FAIL: no link log"; exit 1; }

# ---- the checker: pure text asserts over LINK.EXE's %LINK-I-GPDISP diagnostics.
# A %LINK-I-GPDISP line is:
#   ... proc=NAME K=0xKK ... weakover=W raw_k=0xRR (patched -K, signed-split)
check_locals () {
    local f="$1" gpdisp_of raw_of wk_of undef exitc
    undef=$(grep -c 'GPDISPUNDEF' "$f" || true)
    exitc=$(sed -n 's/^LINK_EXIT=\([0-9]*\)$/\1/p' "$f" | tail -1)

    [ "$undef" -eq 0 ]      || { echo "FAIL: $undef %LINK-F-GPDISPUNDEF (a proc's K did not resolve)"; return 1; }
    [ "${exitc:-1}" -eq 0 ] || { echo "FAIL: LINK.EXE exited ${exitc:-?} (shareable did not link)";     return 1; }

    # (2) static_helper (LOCAL) resolved to a NONZERO K.
    local sh_k
    sh_k=$(sed -n 's/.*proc=static_helper K=0x\([0-9a-fA-F]*\) .*/\1/p' "$f" | head -1)
    [ -n "$sh_k" ]          || { echo "FAIL: no %LINK-I-GPDISP for the LOCAL proc static_helper"; return 1; }
    [ "$sh_k" != "0" ]      || { echo "FAIL: static_helper resolved to K=0 (stub/zero operand read)"; return 1; }

    # (3) leaf_extern (PT_NULL-extern leaf) resolved.
    grep -q 'proc=leaf_extern K=0x' "$f" || { echo "FAIL: no %LINK-I-GPDISP for the PT_NULL-extern leaf leaf_extern"; return 1; }

    # (4) shared_proc: the WEAK-def site (weakover=1) redirected to the STRONG K.
    #     Grab the weakover=1 line's K and raw_k, and a weakover=0 (strong) line's K.
    local w_line s_k w_k w_raw
    w_line=$(grep 'proc=shared_proc' "$f" | grep 'weakover=1' | head -1)
    [ -n "$w_line" ]        || { echo "FAIL: shared_proc has no weakover=1 site (weak def not overridden)"; return 1; }
    w_k=$(  printf '%s\n' "$w_line" | sed -n 's/.* K=0x\([0-9a-fA-F]*\) .*/\1/p')
    w_raw=$(printf '%s\n' "$w_line" | sed -n 's/.*raw_k=0x\([0-9a-fA-F]*\) .*/\1/p')
    s_k=$(grep 'proc=shared_proc' "$f" | grep 'weakover=0' | sed -n 's/.* K=0x\([0-9a-fA-F]*\) .*/\1/p' | head -1)
    [ -n "$s_k" ]           || { echo "FAIL: shared_proc has no strong (weakover=0) def site"; return 1; }
    [ "$w_raw" != "$w_k" ]  || { echo "FAIL: shared_proc weak site raw_k==K (not actually redirected)"; return 1; }
    [ "$w_k" = "$s_k" ]     || { echo "FAIL: shared_proc weak site K=0x$w_k != strong def K=0x$s_k (bound to wrong def)"; return 1; }
    echo "OK (static_helper K=0x$sh_k; shared_proc weak raw_k=0x$w_raw -> strong K=0x$w_k)"
    return 0
}

echo "== [3/3] assert the resolution, then PROVE the gate can fail =="
echo "-- %LINK-I-GPDISP diagnostics --"; grep 'LINK-I-GPDISP,' "$LOG" || true
echo "-- assertion on the real link (must PASS) --"
if ! check_locals "$LOG"; then echo "REAL LINK FAILED ASSERTIONS"; exit 1; fi

fail_expected () {  # $1=name  $2=mutated log
    if check_locals "$2" >/dev/null 2>&1; then
        echo "FAIL: mutant '$1' was ACCEPTED — the gate cannot detect this break"; exit 1
    fi
    echo "OK: mutant '$1' correctly REJECTED"
}
# M1: drop the LOCAL proc's resolution (simulate the pre-fix GPDISPUNDEF path)
grep -v 'proc=static_helper' "$LOG" > "$WORK/m1.log"
fail_expected "local proc unresolved"       "$WORK/m1.log"
# M2: zero the LOCAL proc's K (simulate reading the old always-zero operand field)
sed 's/\(proc=static_helper K=0x\)[0-9a-fA-F]*/\10/' "$LOG" > "$WORK/m2.log"
fail_expected "local K forced to 0"          "$WORK/m2.log"
# M3: bind the weak site to its OWN weak K (simulate a missing strong-over-weak redirect)
awk '/proc=shared_proc/ && /weakover=1/ {
        raw=""; if (match($0,/raw_k=0x[0-9a-fA-F]+/)) raw=substr($0,RSTART+7,RLENGTH-7);
        sub(/K=0x[0-9a-fA-F]+/, "K=0x" raw); }
     {print}' "$LOG" > "$WORK/m3.log"
fail_expected "weak site bound to weak K"    "$WORK/m3.log"

echo
echo "PASS: LOCAL/static + PT_NULL-extern + weak-overridden module-GP K all resolve"
echo "      (zero GPDISPUNDEF; weak redirected to the surviving strong def); mutants rejected."
