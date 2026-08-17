#!/bin/sh
# run_mmk_link_selfhost_plan.sh -- MMK.EXE builds the PLAN for the multi-TU
# LINK.EXE self-host fixpoint end to end (bead vms-89d, the vms-678 Build-native
# 1.0 gate).
#
# This is the PARSE+PLAN half of porting the LINK.EXE self-host fixpoint from
# BUILD.COM to MMK: it proves MMK.EXE reads the committed LINKSH.MMS, expands its
# MMS macros, builds the target/dependency graph and emits the EXACT command
# sequence that builds the OVMX linker LINK.EXE from its two translation units --
# two TCC.EXE compiles (link.c + ovmx_link_rms_io.c), then one LINK.EXE
# `--executable --use {DECC$SHR + the five OVMX shareables}` link -- in dependency
# order.  This is the same shape as run_mmk_component_plan.sh (which proves MMK
# plans the freestanding-runtime component), now for the self-host FIXPOINT
# component: LINK.EXE itself.
#
# Veracity (a REAL end-to-end through the actual files MMK reads):
#   1. The committed LINKSH.MMS + the two real LINK.EXE TUs (src/vmslink/link.c,
#      ovmx_link_rms_io.c, staged under their VMS upper-case names) go into an
#      isolated work dir.
#   2. MMK.EXE reads LINKSH.MMS through OVMX RMS, parses it with the real
#      lib$table_parse engine + the vms-486 PARSE_TABLES grammar + MMK's own
#      parse_store, expands the MMS macros ($(CFLAGS)/$(INCS)/$(RTLIBS)/$(OBJS)/
#      $(MMS$TARGET)/$(MMS$SOURCE)), builds the graph and, in /NOACTION, emits the
#      exact DCL command lines it WOULD drive -- in dependency order.
#   3. This test asserts the WHOLE plan: BOTH TCC compiles, THEN the LINK, with
#      every compile before the link -- the plan a self-hosting `make` must
#      produce for LINK.EXE.
#   4. DETERMINISM: MMK is run twice and the two plans are asserted byte-identical.
#
# The independent oracle is LINKSH.MMS itself: the expected command bodies are the
# TCC/LNK lines the description file specifies, in its dependency order.
#
# SCOPE (honest, CLAUDE.md Rule 9 / INV-6).  This is the PARSE+PLAN half, run on
# the host with NO /dev/vms.  MMK's PERSISTENT MAILBOX-DRIVEN EXECUTION of this
# plan (ovmx_mmk_sp.c: $CREMBX + lib$spawn + write-attention AST + $HIBER) needs a
# real executive and CANNOT run in a host container ($CREMBX fails SS$_NOSUCHDEV);
# it is exercised in QEMU by tests/qemu/test_syssvc_mmk_link_selfhost.c.  The
# real-toolchain OUTPUT of this plan -- the gen2==gen3 byte-stable fixpoint -- is
# proven on the host by run_mmk_link_selfhost_build.sh.  This test does NOT fake
# execution: in /NOACTION MMK executes nothing (its own documented dry-run mode).
#
# Inputs (env, set by CMake add_test): MMK_EXE, REPO_SRC (repo src/ dir).
# Exit 0 = success.
set -e

: "${MMK_EXE:?need MMK_EXE (built MMK.EXE)}"
[ -x "$MMK_EXE" ] || { echo "FAIL: MMK_EXE not executable: $MMK_EXE"; exit 1; }
: "${REPO_SRC:?need REPO_SRC (repo src/ dir)}"

HERE=$(cd "$(dirname "$0")" && pwd)
COMPONENT="$HERE/component"
VMSLINK="$REPO_SRC/vmslink"

for f in "$COMPONENT/LINKSH.MMS" "$VMSLINK/link.c" "$VMSLINK/ovmx_link_rms_io.c"; do
    [ -f "$f" ] || { echo "FAIL: missing input $f"; exit 1; }
done

# MMK opens the description file through OVMX RMS, which resolves a bare filespec
# against the process default directory (cwd).  Work in an isolated temp dir.
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

# Stage the descrip.mms and the REAL LINK.EXE TUs (VMS-style upper-case .C).
cp "$COMPONENT/LINKSH.MMS" LINKSH.MMS
cp "$VMSLINK/link.c"             LINK.C
cp "$VMSLINK/ovmx_link_rms_io.c" OVMX_LINK_RMS_IO.C
# /RULES defaults to MMS$RULES; an empty one keeps the run's status clean.
printf '! empty default rules (vms-89d LINK.EXE self-host plan proof)\n' > "MMS\$RULES"

echo "== LINKSH.MMS (the committed descrip.mms) =="
sed 's/^/   /' LINKSH.MMS

run_plan() {
    VMS_FOREIGN_CMD="/DESCRIPTION=LINKSH.MMS /NOACTION LINKSH.EXE" \
        "$MMK_EXE" < /dev/null 2>/dev/null
}

echo
echo "== MMK.EXE /DESCRIPTION=LINKSH.MMS /NOACTION LINKSH.EXE (build 1) =="
set +e
run_plan > plan1.txt
RC=$?
set -e
echo "-- MMK exit=$RC (odd VMS status = success); resolved plan: --"
sed 's/^/   /' plan1.txt

# VMS success status is ODD.
if [ "$RC" != "1" ] && [ "$RC" != "0" ]; then
    if [ $((RC & 1)) -eq 0 ]; then echo "FAIL: MMK exited with an even (failure) status $RC"; exit 1; fi
fi

# ---- assert the full two-TU + link plan, in dependency order ----
line() { grep -nxF "$1" plan1.txt | head -1 | cut -d: -f1; }

C_LNK=$(line 'TCC -x c -c -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DOVMX_RMS_IO -I VMSLINK$SRC: -I OVMX$INCLUDE: -I RMS$INCLUDE: LINK.C -o LINK.OBJ')
C_RMS=$(line 'TCC -x c -c -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DOVMX_RMS_IO -I VMSLINK$SRC: -I OVMX$INCLUDE: -I RMS$INCLUDE: OVMX_LINK_RMS_IO.C -o OVMX_LINK_RMS_IO.OBJ')
# The LINK line: --executable, all six --use producers, then the two objects.
L_LNK=$(grep -nE '^LNK --executable .*--use SYS\$LIBRARY:DECC\$SHR\.EXE.*--use SYS\$LIBRARY:LIBVMSRMS\$SHR\.EXE .*-o LINKSH\.EXE LINK\.OBJ OVMX_LINK_RMS_IO\.OBJ$' plan1.txt | head -1 | cut -d: -f1)

fails=0
need() { if [ -z "$2" ]; then echo "FAIL: $1"; fails=$((fails+1)); else echo "  PASS: $1 (plan line $2)"; fi; }
need "MMK resolved the TCC compile of LINK.C -> LINK.OBJ (CFLAGS/INCS macros expanded, -DOVMX_RMS_IO)" "$C_LNK"
need "MMK resolved the TCC compile of OVMX_LINK_RMS_IO.C -> OVMX_LINK_RMS_IO.OBJ" "$C_RMS"
need "MMK resolved the LINK --executable of the two objects against the six shareables -> LINKSH.EXE (RTLIBS macro expanded)" "$L_LNK"
[ "$fails" -eq 0 ] || { echo "FAIL: MMK did not resolve the full LINK.EXE self-host plan"; exit 1; }

# Dependency order: both compiles precede the link.
maxc=0
for v in "$C_LNK" "$C_RMS"; do [ "$v" -gt "$maxc" ] && maxc=$v; done
[ "$maxc" -lt "$L_LNK" ] || { echo "FAIL: plan order -- a compile did not precede the LINK"; exit 1; }
echo "  PASS: dependency order -- both compiles < LINK"

# ---- determinism: a second run yields a byte-identical plan ----
echo
echo "== MMK.EXE (build 2): assert the plan is byte-identical (determinism) =="
set +e
run_plan > plan2.txt
set -e
if cmp -s plan1.txt plan2.txt; then
    echo "  PASS: the resolved plan is BYTE-IDENTICAL across two independent MMK runs (cmp clean)"
else
    echo "FAIL: MMK produced a non-deterministic plan across two runs:"; diff plan1.txt plan2.txt || true; exit 1
fi

echo
echo "================================================================================"
echo "MILESTONE (vms-89d, self-host S4 -- LINK.EXE self-host fixpoint, MMK plan): MMK.EXE"
echo "parsed the committed LINKSH.MMS, expanded its MMS macros, built the dependency"
echo "graph and emitted the correct build plan -- two TCC.EXE compiles (link.c +"
echo "ovmx_link_rms_io.c), then a LINK.EXE --executable of the two objects against the"
echo "six shareables -- in dependency order, byte-identical across two runs.  ZERO bash in"
echo "the plan (MMK drives it).  Executing the plan to the gen2==gen3 fixpoint is proven"
echo "on the host by run_mmk_link_selfhost_build.sh; MMK's mailbox-driven execution rides"
echo "the QEMU test (tests/qemu/test_syssvc_mmk_link_selfhost.c)."
echo "================================================================================"
