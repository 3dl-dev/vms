#!/bin/bash
# run_install_test.sh — end-to-end INSTALL utility test (bead vms-913.5).
#
# INSTALL ADD registers an image -> LIST shows it (with qualifiers) ->
# the standalone known_images lookup module finds it by SONAME via
# mmap(MAP_SHARED) on the real database INSTALL wrote -> REMOVE
# de-registers it -> LIST no longer shows it -> the lookup module no
# longer finds it (would fall back to filesystem search per
# docs/design-image-activation.md section 4).
#
# INSTALL_EXE and LOOKUP_HELPER are injected by src/install/CMakeLists.txt
# via ENVIRONMENT on the ctest 'install_utility' test.
#
# NOTE: like the existing tests/dcl SYSMAN/SYSGEN tests, this exercises the
# real SYS$SYSTEM/SYS$SHARE paths under /vms (VMS_SYSTEM_DIR / VMS_LIBRARY_DIR
# in ovmx_layout.h are not test-overridable — same convention already used
# by tests/dcl/test_sysman.sh against SYSMGR_DIR).

set -eu

INSTALL_EXE="${INSTALL_EXE:?INSTALL_EXE not set}"
LOOKUP_HELPER="${LOOKUP_HELPER:?LOOKUP_HELPER not set}"

SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
DB="$SYSEXE/VMS\$KNOWN_IMAGES.DAT"

mkdir -p "$SYSLIB"

# Start from a clean DB so this test is repeatable across runs.
rm -f "$DB"
echo "dummy shareable image" > "$SYSLIB/TESTLIB\$SHR.EXE"

echo "== INSTALL ADD =="
OUT=$("$INSTALL_EXE" ADD 'SYS$SHARE:TESTLIB$SHR.EXE' /OPEN /SHARED /HEADER_RESIDENT)
echo "$OUT"
echo "$OUT" | grep -q "INSTALL-I-ADDED" || { echo "FAIL: ADD did not report success"; exit 1; }

echo
echo "== INSTALL LIST =="
OUT=$("$INSTALL_EXE" LIST)
echo "$OUT"
echo "$OUT" | grep -q 'TESTLIB$SHR.EXE' || { echo "FAIL: LIST does not show added image"; exit 1; }
echo "$OUT" | grep -q "OPEN"            || { echo "FAIL: LIST does not show OPEN flag"; exit 1; }
echo "$OUT" | grep -q "SHARED"          || { echo "FAIL: LIST does not show SHARED flag"; exit 1; }
echo "$OUT" | grep -q "HEADER_RESIDENT" || { echo "FAIL: LIST does not show HEADER_RESIDENT flag"; exit 1; }

echo
echo "== INSTALL LIST /FULL =="
OUT=$("$INSTALL_EXE" LIST /FULL)
echo "$OUT"
echo "$OUT" | grep -q "$SYSLIB/TESTLIB\$SHR.EXE" || { echo "FAIL: LIST /FULL does not show resolved path"; exit 1; }

echo
echo "== standalone lookup module finds the INSTALL-registered image (mmap MAP_SHARED) =="
"$LOOKUP_HELPER" "$DB" 'TESTLIB$SHR.EXE'

echo
echo "== INSTALL REMOVE =="
OUT=$("$INSTALL_EXE" REMOVE 'SYS$SHARE:TESTLIB$SHR.EXE')
echo "$OUT"
echo "$OUT" | grep -q "INSTALL-I-REMOVED" || { echo "FAIL: REMOVE did not report success"; exit 1; }

echo
echo "== INSTALL LIST after REMOVE (must not show the image) =="
OUT=$("$INSTALL_EXE" LIST)
echo "$OUT"
if echo "$OUT" | grep -q 'TESTLIB$SHR.EXE'; then
    echo "FAIL: image still listed after REMOVE"
    exit 1
fi

echo
echo "== lookup module no longer finds the de-registered image =="
if "$LOOKUP_HELPER" "$DB" 'TESTLIB$SHR.EXE' 2>/dev/null; then
    echo "FAIL: lookup still finds a removed image"
    exit 1
fi
echo 'OK: lookup no longer finds TESTLIB$SHR.EXE (IMGACT would fall back to filesystem search)'

rm -f "$DB" "$SYSLIB/TESTLIB\$SHR.EXE"

echo
echo "ALL INSTALL UTILITY TESTS PASSED"
