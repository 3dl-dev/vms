#!/bin/bash
# run_install_test.sh -- INSTALL with no executive fails honestly (rd vms-220).
#
# The known file list is the executive's: INSTALL ADD/REPLACE/REMOVE/LIST are
# VMS_IOCTL_KFE requests. On the host there is no /dev/vms, so every one of
# them must fail -- %INSTALL-E-FAIL naming NOSUCHDEV, a nonzero exit, and no
# "added"/"removed" claim -- and INSTALL must write no on-disk substitute (the
# retired SYS$SYSTEM:VMS$KNOWN_IMAGES.DAT). The executive side is proven
# against a real /dev/vms by tests/qemu/test_syssvc_kfe.c.
#
# INSTALL_EXE is injected by src/install/CMakeLists.txt.
set -u
INSTALL_EXE="${INSTALL_EXE:?INSTALL_EXE not set}"
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
DB="$SYSEXE/VMS\$KNOWN_IMAGES.DAT"
fails=0
die() { echo "FAIL: $1"; fails=$((fails + 1)); }

if [ -e /dev/vms ]; then
    echo "FAIL: this host has /dev/vms -- the no-executive case cannot be tested here"
    exit 1
fi
mkdir -p "$SYSLIB" "$SYSEXE"
rm -f "$DB"
echo "dummy shareable image" > "$SYSLIB/TESTLIB\$SHR.EXE"

run() {   # <label> <args...>: must fail, honestly
    local label="$1"; shift
    local out rc
    out=$("$INSTALL_EXE" "$@" 2>&1); rc=$?
    echo "== INSTALL $* -> rc=$rc"; echo "$out"
    [ "$rc" -ne 0 ] || die "$label: exited 0 with no executive"
    echo "$out" | grep -q '%INSTALL-E-FAIL' || die "$label: no %INSTALL-E-FAIL"
    echo "$out" | grep -q 'NOSUCHDEV' || die "$label: the failure does not name NOSUCHDEV"
    echo "$out" | grep -qiE 'ADDED|REMOVED|installed$|NOIMAGES' && die "$label: claims a result it could not have"
}
run ADD     ADD 'SYS$SHARE:TESTLIB$SHR.EXE' /OPEN /SHARED
run REPLACE REPLACE 'SYS$SHARE:TESTLIB$SHR.EXE' /OPEN
run LIST    LIST /FULL
run REMOVE  REMOVE 'SYS$SHARE:TESTLIB$SHR.EXE'
[ -e "$DB" ] && die "INSTALL wrote an on-disk database ($DB)"

rm -f "$SYSLIB/TESTLIB\$SHR.EXE"
[ "$fails" -eq 0 ] && { echo "PASS: INSTALL with no executive fails honestly (NOSUCHDEV), claims nothing, writes no database"; exit 0; }
exit 1
