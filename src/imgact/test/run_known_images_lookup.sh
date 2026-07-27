#!/bin/sh
# run_known_images_lookup.sh — proves IMGACT.EXE's DT_NEEDED search consults
# the Known Image Database BEFORE the filesystem search (bead vms-30d, wiring
# known_images_lookup() from the standalone vms-913.5 module into
# src/imgact/imgact.c's load_needed(), docs/design-image-activation.md §4
# Priority 1).
#
# Reuses the vms-913.2 proof pair (test_lib.c / test_prog.c: DT_NEEDED on
# LIBTEST$SHR.EXE, PT_INTERP=IMGACT.EXE) but installs the shareable image at
# a path that is NOT the hardcoded SYS$SHARE fallback
# (/vms/SYS0/SYSCOMMON/SYSLIB) — a directory the fallback path search can
# never see. A hand-built VMS$KNOWN_IMAGES.DAT registers the real path.
#
#   PART A (hit):  DB registers LIBTEST$SHR.EXE -> KNOWNONLY/LIBTEST$SHR.EXE.
#                  SYS$SHARE has no copy. Activation must still succeed
#                  ("IMGACT-TEST: PASS", exit 0) — the only way that can
#                  happen is the Known Image DB lookup, proving it runs and
#                  short-circuits before any filesystem search of SYS$SHARE.
#   PART B (miss): remove the DB (simulates "not yet installed" / absent).
#                  The same test_prog, unchanged, must now fail with
#                  %IMGACT-F-IMGNOTFND: with no DB and no copy in SYS$SHARE,
#                  the Priority-2 fallback correctly finds nothing. This is
#                  the "falls back to the fs search on a miss" half of the
#                  done condition (the fs search runs and correctly fails,
#                  since the image genuinely isn't at the fallback path).
#
# Runs INSIDE an Alpine (native musl) container. Exit 0 only if both parts
# behave as described.

set -e

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB          # hardcoded fallback path — stays empty
KNOWNDIR=/vms/KNOWNONLY                    # only reachable via the Known Image DB
LIB='LIBTEST$SHR.EXE'
INTERP="$SYSEXE/IMGACT.EXE"
DB="$SYSEXE/VMS\$KNOWN_IMAGES.DAT"

SRC=$(cd "$(dirname "$0")/.." && pwd)      # src/imgact
cd "$SRC"

echo "== build IMGACT.EXE (with known-image DB lookup wired in) =="
make CC="${CC:-gcc}" clean >/dev/null 2>&1 || true
make CC="${CC:-gcc}"
mkdir -p "$SYSEXE" "$SYSLIB" "$KNOWNDIR"
cp IMGACT.EXE "$INTERP"
rm -f "$SYSLIB/$LIB"   # make certain the fallback path really is empty

echo "== build test shareable image ($LIB), installed ONLY at $KNOWNDIR =="
$CC -std=gnu11 -O2 -Wall -shared -fPIC -mtls-dialect=desc -nostdlib \
    -Wl,--hash-style=sysv -Wl,-z,norelro -Wl,-soname,"$LIB" \
    -o "$LIB" test/test_lib.c
cp "$LIB" "$KNOWNDIR/$LIB"

echo "== build test executable (PT_INTERP=$INTERP, DT_NEEDED=$LIB) =="
$CC -std=gnu11 -O2 -Wall -no-pie -nostdlib -ffreestanding -fno-stack-protector \
    -Wl,--dynamic-linker="$INTERP" -Wl,--hash-style=sysv -Wl,-z,norelro \
    -Wl,--allow-shlib-undefined -Wl,-e,_start \
    -o test/known_images_prog test/test_prog.c -L"$KNOWNDIR" -l:"$LIB"

echo "== write VMS\$KNOWN_IMAGES.DAT registering $LIB -> $KNOWNDIR/$LIB =="
cat > /tmp/write_kfe.c <<EOF
#include "known_images.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(void) {
    struct kfe_file db;
    memset(&db, 0, sizeof(db));
    db.magic = KFE_MAGIC;
    db.version = KFE_VERSION;
    db.count = 1;
    strncpy(db.entries[0].soname, "$LIB", sizeof(db.entries[0].soname) - 1);
    strncpy(db.entries[0].path, "$KNOWNDIR/$LIB", sizeof(db.entries[0].path) - 1);
    db.entries[0].flags = KFE_F_SHARED;
    db.entries[0].gsmatch_op = KFE_GSMATCH_ALWAYS;
    FILE *fp = fopen("$DB", "wb");
    if (!fp) { perror("fopen"); return 1; }
    if (fwrite(&db, sizeof(db), 1, fp) != 1) { perror("fwrite"); fclose(fp); return 1; }
    fclose(fp);
    return 0;
}
EOF
$CC -std=gnu11 -O2 -Wall -I"$SRC" -o /tmp/write_kfe /tmp/write_kfe.c
/tmp/write_kfe

echo
echo "== PART A (hit): $LIB exists ONLY via the Known Image DB, not SYS\$SHARE =="
[ ! -e "$SYSLIB/$LIB" ] || { echo "FAIL: test setup bug -- $LIB must not exist in SYS\$SHARE"; exit 1; }
set +e
OUT=$(./test/known_images_prog); RC=$?
set -e
echo "stdout/stderr: $OUT"
echo "exit code: $RC"
case "$OUT" in
    *"IMGACT-TEST: PASS"*) ;;
    *) echo "FAIL: expected PASS -- known-image DB lookup did not resolve $LIB"; exit 1 ;;
esac
[ "$RC" -eq 0 ] || { echo "FAIL: expected exit 0"; exit 1; }
echo "PART A OK: known-image DB short-circuited straight to $KNOWNDIR/$LIB, no SYS\$SHARE copy needed"
echo

echo "== PART B (miss): remove the DB -- same program, SYS\$SHARE still has no copy =="
rm -f "$DB"
set +e
FOUT=$(./test/known_images_prog 2>&1); FRC=$?
set -e
echo "diagnostic: $FOUT"
echo "exit code: $FRC"
case "$FOUT" in
    *"%IMGACT-F-IMGNOTFND"*) ;;
    *) echo "FAIL: expected %IMGACT-F-IMGNOTFND once the DB is gone"; exit 1 ;;
esac
[ "$FRC" -ne 0 ] || { echo "FAIL: expected nonzero exit"; exit 1; }
echo "PART B OK: with no DB, IMGACT correctly fell back to the SYS\$SHARE search and (correctly) failed"

rm -f test/known_images_prog "$LIB" "$KNOWNDIR/$LIB" /tmp/write_kfe /tmp/write_kfe.c

echo
echo "ALL KNOWN-IMAGE DATABASE LOOKUP CHECKS PASSED"
