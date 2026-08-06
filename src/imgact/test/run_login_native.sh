#!/bin/sh
# run_login_native.sh — sibling of run_dcl_native.sh (vms-c39, pillar vms-ade):
# build LOGINOUT.EXE -- the OVMX login program -- as a VMS-native EXECUTABLE
# image via LINK.EXE, activate it through IMGACT.EXE, and drive it against a
# REAL SYSUAF.DAT with the shipped SYSTEM/MANAGER credentials -- NO ld / NO
# ld.so anywhere in the LOGINOUT half of the chain.
#
# WHY A SIBLING AND NOT A THIRD COPY OF THE GRAPH BUILD: the producer-graph
# steps (IMGACT.EXE, LINK.EXE, DECC$SHR..LIBVMSRMS$SHR) are IDENTICAL to
# run_dcl_native.sh's and now live once, in lib_build_graph.sh, sourced by
# both scripts (vms-c39 extended run_dcl_native.sh to source it too, rather
# than leaving two copies to drift -- see mk_libvms_shr.sh's LIST comment for
# what that drift costs). What is NOT shared is what happens after the graph:
# DCL's harness feeds a script straight to DCL.EXE; this harness authenticates
# through LOGINOUT.EXE first, over a REAL SYSUAF.DAT (the one distro/rootfs
# ships, SHA256 hash and all -- not a fabricated test fixture), and that is a
# different enough shape (stdin credential piping, SYSUAF setup, a DIFFERENT
# and CORRECT terminal assertion -- see below) to earn its own script rather
# than growing conditionals into the DCL one.
#
# WHAT THIS SCRIPT PROVES:
#   1. LOGINOUT.EXE (tools/vms_login.c, the ONE TU) links as a VMS-native
#      ET_DYN executable via `LINK.EXE --executable --use {DECC$SHR + the six
#      OVMX shareables}` (mk_loginout.sh) -- same shape as DCL.EXE: PT_INTERP
#      = IMGACT.EXE, all cross-image imports bound, STRICT link.
#   2. IMGACT.EXE activates LOGINOUT.EXE (crt0 recovers argc/argv, calls
#      main()) with NO ld / NO ld.so.
#   3. Once activated, the REAL sysuaf_lookup()/sysuaf_authenticate() cross-
#      image calls (bound to LIBVMS$SHR, not stubbed) authenticate SYSTEM
#      against the REAL SHA256 hash distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/
#      SYSUAF.DAT ships for password MANAGER -- i.e. the Username:/Password:
#      prompts, the SYSUAF file I/O, and the hash comparison all run for
#      real, inside the VMS-native image, with zero mocking.
#
# WHAT THIS SCRIPT DOES *NOT* PROVE, AND WHY THAT IS THE CORRECT OUTCOME HERE
# (CLAUDE.md Rule 9), NOT A GAP IN THE TEST:
#   After authentication succeeds, LOGINOUT calls vms_kif_setident() to stamp
#   the identity into the executive over /dev/vms -- and this container has
#   no /dev/vms (no vms.ko; Docker/podman here is BUILD/TEST TOOLING, never a
#   runtime, and none of the three labs [vms-2f3 lab-1, vms-a5c lab-2,
#   vms-e2c lab-Alpha] load a Linux-side vms.ko either -- they run OpenVMS
#   itself). So vms_kif_setident correctly, HONESTLY fails (no /dev/vms to
#   open), and vms_login.c's own "FAILURE IS FATAL" contract (see the long
#   comment above that call in tools/vms_login.c) makes LOGINOUT print
#   %OVMX-F-NOIDENT and _exit(1) -- it does NOT fall through to exec DCL.EXE.
#   THAT IS THE ASSERTION BELOW: a VMS-native LOGINOUT.EXE, given no
#   executive, refuses to hand over a session rather than silently
#   succeeding -- exactly the behavior Rule 9 requires and exactly the
#   behavior a userspace-fallback bug would violate. Proving the SUCCESSFUL
#   post-auth exec into a VMS-native DCL.EXE needs a real /dev/vms, which
#   means a real kernel, which means QEMU -- out of reach for a Docker
#   build-container harness by construction, not by omission. See this
#   item's summary for the receipt (three approaches tried, none available
#   in this sandbox) and the follow-up this leaves for whoever next drives
#   tests/uat/vms_session_qemu.sh (the QEMU harness that ALREADY proves the
#   old ld/ld.so-linked LOGINOUT->DCL chain end-to-end against a real
#   /dev/vms) to point at the VMS-native LOGINOUT.EXE/DCL.EXE instead.
#
# DCL.EXE is also built here (mk_dcl.sh) because LOGINOUT.EXE's execl() target
# must exist on SYS$SYSTEM: for the exec call itself to be reachable at all
# (even though, per the above, it is not reached in THIS container).
#
# LOGIN_EXPECT_LINK defaults to 1 (mirrors DCL_EXPECT_LINK): a link failure
# for either DCL.EXE or LOGINOUT.EXE is a hard FAIL, not a skip.
#
# arm64 musl container only (CLAUDE.md test loop; vms-c39: x86_64 needs
# vms-bdf's separate LINK.EXE-backend proof, tracked separately, not waited
# on here). Needs root to create /vms.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
SRC=$(cd "$IMGACT_DIR/.." && pwd)            # src
REPO=$(cd "$SRC/.." && pwd)                  # repo root
LIBVMSSYS_DIR="$SRC/libvmssys"
VMSPROC_DIR="$SRC/vmsprocess"
VMSLNM_DIR="$SRC/vmslnm"
VMSFS_DIR="$SRC/vmsfs"
LIBVMS_DIR="$SRC/libvms"
VMSRMS_DIR="$SRC/vmsrms"
DCL_DIR="$SRC/vmsdcl"
LIBVMS_INC="$LIBVMS_DIR/include"
LNM_INC="$VMSLNM_DIR/include"
VMSFS_INC="$VMSFS_DIR/include"
RMS_INC="$VMSRMS_DIR/include"
WORK=${WORK:-/tmp/login-native}
rm -rf "$WORK"; mkdir -p "$WORK"

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

LIBC=${LIBC:-/usr/lib/libc.a}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]   || { echo "SKIP-FAIL: no musl libc.a at $LIBC (need arm64 musl container)"; exit 1; }
[ -f "$LIBGCC" ] || { echo "FAIL: no libgcc.a at $LIBGCC"; exit 1; }

# SYS_VEC_EXTRA: LOGINOUT needs vms_kif_setident (identity establishment),
# which DCL never calls and run_dcl_native.sh's LIBVMSSYS$SHR vector
# therefore never exported. Appended, not reordered -- the vector contract
# (mk_vmsprocess_shr.sh's comment) is append-only.
SYS_VEC_EXTRA="vms_kif_setident=PROCEDURE"
. "$HERE/lib_build_graph.sh"
build_producer_graph

echo
echo "== compile the real src/vmsdcl (21 TUs) + src/vmsqueue, link DCL.EXE =="
sh "$LINK_DIR/mk_dcl.sh" "$WORK/LINK.EXE" "$SYSEXE/DCL.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$SYSLIB/LIBVMSSYS\$SHR.EXE" \
    "$DCL_DIR" "$SRC" >/dev/null
readelf -lW "$SYSEXE/DCL.EXE" | grep -q 'INTERP' || { echo "FAIL: DCL.EXE has no PT_INTERP (IMGACT)"; exit 1; }
echo "-- DCL.EXE (LOGINOUT's execl() target) linked VMS-native --"

echo
echo "== link LOGINOUT.EXE VMS-native (LINK.EXE --executable --use the seven shareables) =="
set +e
sh "$LINK_DIR/mk_loginout.sh" "$WORK/LINK.EXE" "$SYSEXE/LOGINOUT.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$SYSLIB/LIBVMSSYS\$SHR.EXE" \
    "$REPO/tools/vms_login.c" "$SRC" 2>"$WORK/link.err"
LRC=$?
set -e
echo "-- LINK.EXE --executable exit=$LRC; message: --"
tail -3 "$WORK/link.err" | sed 's/^/   /'

if [ "$LRC" -ne 0 ]; then
    if [ "${LOGIN_EXPECT_LINK:-1}" = "1" ]; then
        echo "FAIL: LOGINOUT.EXE executable link failed (regression). See link.err above."
        exit 1
    fi
    echo "SKIP (LOGIN_EXPECT_LINK=0): LOGINOUT.EXE link failed but the assertion is disabled."
    exit 2
fi
readelf -lW "$SYSEXE/LOGINOUT.EXE" | grep -q 'INTERP' || { echo "FAIL: LOGINOUT.EXE has no PT_INTERP (IMGACT)"; exit 1; }
readelf -hW "$SYSEXE/LOGINOUT.EXE" | grep -q 'DYN (' || { echo "FAIL: LOGINOUT.EXE is not ET_DYN"; exit 1; }
echo "-- LOGINOUT.EXE: VMS-native ET_DYN, PT_INTERP=IMGACT.EXE --"

echo
echo "== seed a REAL SYSUAF.DAT (the one distro/rootfs ships -- SYSTEM/MANAGER, real SHA256 hash) =="
cp "$REPO/distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/SYSUAF.DAT" "$SYSEXE/SYSUAF.DAT"
grep -q '^SYSTEM|' "$SYSEXE/SYSUAF.DAT" || { echo "FAIL: shipped SYSUAF.DAT has no SYSTEM row"; exit 1; }

echo
echo "== activate LOGINOUT.EXE through IMGACT.EXE, authenticate SYSTEM/MANAGER =="
printf 'SYSTEM\nMANAGER\nSHOW TIME\nEXIT\n' > "$WORK/session_input"
set +e
"$SYSEXE/LOGINOUT.EXE" < "$WORK/session_input" > "$WORK/session.out" 2>&1
RC=$?
set -e
echo "-- LOGINOUT session output: --"; sed 's/^/   /' "$WORK/session.out"
echo "exit code = $RC"

grep -q 'Username:' "$WORK/session.out" || { echo "FAIL: no Username: prompt -- LOGINOUT did not run its console_login path"; exit 1; }
grep -q 'User authorization failure' "$WORK/session.out" && { echo "FAIL: SYSTEM/MANAGER against the REAL shipped SYSUAF.DAT was refused -- sysuaf_lookup/sysuaf_authenticate did not bind/run correctly cross-image"; exit 1; }
echo "-- SYSTEM authenticated against the real SYSUAF.DAT hash (no mock) --"

# THE CORRECT OUTCOME IN THIS CONTAINER (see header): no /dev/vms means
# vms_kif_setident MUST fail, and LOGINOUT MUST refuse the session honestly
# rather than exec into DCL.EXE anyway. Assert exactly that -- the opposite
# assertion (a DCL prompt reached, or SHOW TIME's output) would mean a
# silent userspace fallback exists, which is the bug class Rule 9 forbids.
grep -q '%OVMX-F-NOIDENT' "$WORK/session.out" || { echo "FAIL: LOGINOUT did not report %OVMX-F-NOIDENT -- expected an honest refusal with no /dev/vms present"; exit 1; }
[ "$RC" -ne 0 ] || { echo "FAIL: LOGINOUT exited 0 with no /dev/vms present -- it must refuse, not silently succeed (Rule 9)"; exit 1; }
grep -qE '^ [0-9]{2}-[A-Z]{3}-[0-9]{4}' "$WORK/session.out" && { echo "FAIL: DCL's SHOW TIME output appears -- LOGINOUT execed into DCL.EXE despite no established identity"; exit 1; }
echo "-- LOGINOUT correctly refused the session (%OVMX-F-NOIDENT, exit $RC) -- no silent fallback, Rule 9 held --"

echo
echo "RESULT (vms-c39): LOGINOUT.EXE links VMS-native (LINK.EXE --executable --use the"
echo "seven OVMX shareables, same graph as DCL.EXE), activates through IMGACT.EXE with"
echo "NO ld / NO ld.so, and authenticates SYSTEM against the real shipped SYSUAF.DAT"
echo "entirely inside the VMS-native image. With no /dev/vms in this container it"
echo "correctly refuses the session instead of faking success (Rule 9). The successful"
echo "post-auth exec into DCL.EXE needs a real /dev/vms (QEMU) -- see this item's"
echo "summary for the receipt of what was tried in this sandbox and what is left for"
echo "the QEMU harness (tests/uat/vms_session_qemu.sh) to extend."
