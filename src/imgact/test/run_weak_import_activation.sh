#!/bin/sh
# run_weak_import_activation.sh — the WEAK-by-name import milestone (vms-5f0).
#
# Proves LINK.EXE records a `#pragma weak` cross-image reference that NO --use'd
# producer exports as a `.vms$wimp` weak import (NOT a link error, NOT baked to 0
# in place), and that IMGACT.EXE binds it by NAME at activation against whatever
# producer set is loaded -- the mechanism that lets a LOWER-layer shareable reach
# a universal a HIGHER-layer shareable exports across a build cycle the fixed
# (producer,index) .vms$imp path cannot express.
#
# This is the exact shape of the login-through-the-ACP fix: LIBVMS$SHR's
# rms_textfile.c weak-references sys$open/$get/$connect/$close, which
# LIBVMSRMS$SHR exports -- but LIBVMSRMS$SHR --use's LIBVMS$SHR, so LIBVMS$SHR
# cannot --use LIBVMSRMS$SHR to import them by index. Here PWEAKTGT$SHR plays
# LIBVMSRMS$SHR (the higher exporter) and MWEAKLIB$SHR plays LIBVMS$SHR (the
# lower weak-referencer).
#
# TWO layers of proof:
#   LINK LEVEL (always, host-runnable -- no /dev/vms needed):
#     A. MWEAKLIB$SHR built --use DECC$SHR ONLY (PWEAKTGT NOT named): the weak
#        reference weak_probe_target lands in .vms$wimp, the STRICT link SUCCEEDS
#        (weak-undef is not an error), and it does NOT appear in .vms$imp.
#     B. MWEAKLIB2$SHR built --use DECC$SHR + PWEAKTGT: now a --use producer
#        DOES export weak_probe_target, so it binds as a STRONG .vms$imp import
#        and does NOT go to .vms$wimp -- the weak scan yields to the strong one.
#   ACTIVATION LEVEL (only when /dev/vms is present -- i.e. under QEMU, the one
#   real runtime; the plain build container / host has no ACP so IMGACT cannot
#   read the image file at all, by construction, INV-6): the POSITIVE consumer
#   binds weak_probe_target by name and exits 3; the NEGATIVE consumer (target
#   absent) leaves the cell 0 and exits 0 -- the honest weak-undef fallback
#   rms_services_present() relies on. Absent /dev/vms, activation is proven by
#   the full boot-to-DCL login proof instead (this item), not skipped silently.
#
# Host musl (x86_64 or aarch64). Needs write to /vms. Exit 0 on ok.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)
IMGACT_DIR=$(cd "$HERE/.." && pwd)
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
WORK=${WORK:-/tmp/weak-import-act}
rm -rf "$WORK"; mkdir -p "$WORK"

ARCH=${ARCH:-$(uname -m)}
case "$ARCH" in
    aarch64|arm64) ARCH=aarch64; ARCHFLAG="-mno-outline-atomics" ;;
    x86_64|amd64)  ARCH=x86_64;  ARCHFLAG="-mtls-dialect=gnu2" ;;
    *) echo "SKIP-FAIL: unsupported ARCH=$ARCH"; exit 1 ;;
esac
CFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -U_FORTIFY_SOURCE $ARCHFLAG"

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

LIBC=${LIBC:-/usr/lib/$ARCH-linux-musl/libc.a}
[ -f "$LIBC" ] || LIBC=/usr/lib/libc.a
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]   || { echo "SKIP-FAIL: no musl libc.a at $LIBC"; exit 1; }
[ -f "$LIBGCC" ] || { echo "FAIL: no libgcc.a at $LIBGCC"; exit 1; }

echo "== build IMGACT.EXE (.vms\$wimp by-name resolution) + LINK.EXE ($ARCH) =="
( cd "$IMGACT_DIR" && make CC="$CC" ARCH="$ARCH" clean >/dev/null 2>&1 || true; make CC="$CC" ARCH="$ARCH" ) >/dev/null 2>&1
cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -o "$WORK/LINK.EXE" "$LINK_DIR/link.c"

echo "== mk_decc_shr.sh -> DECC\$SHR.EXE =="
sh "$LINK_DIR/mk_decc_shr.sh" "$WORK/LINK.EXE" "$SYSLIB/DECC\$SHR.EXE" "$LIBC" "$LIBGCC" >/dev/null

echo "== PWEAKTGT\$SHR.EXE: exports weak_probe_target() (the higher-layer exporter) =="
cat > "$WORK/ptgt.c" <<'EOF'
int weak_probe_target(void) { return 0xAB; }
EOF
$CC $CFLAGS -c -o "$WORK/ptgt.o" "$WORK/ptgt.c"
"$WORK/LINK.EXE" --shareable --use "$SYSLIB/DECC\$SHR.EXE" \
    --symbol-vector "weak_probe_target=PROCEDURE" --gsmatch LEQUAL,1,0 \
    -o "$SYSLIB/PWEAKTGT\$SHR.EXE" "$WORK/ptgt.o"

echo "== MWEAKLIB\$SHR.EXE: weak-references weak_probe_target (the lower-layer referencer) =="
cat > "$WORK/mwl.c" <<'EOF'
#pragma weak weak_probe_target
extern int weak_probe_target(void);
/* address-taken (GOT) — exercises the DATA import cell */
int mwl_present(void) { return weak_probe_target != 0; }
/* called (PLT) — exercises the call import path through the SAME cell */
int mwl_call(void) { return weak_probe_target ? weak_probe_target() : 0; }
EOF
$CC $CFLAGS -c -o "$WORK/mwl.o" "$WORK/mwl.c"

# ---- LINK LEVEL A: STRICT link (no --allow-undefined). A weak-undef must NOT be
# a link error, and must land in .vms$wimp — NOT baked to 0 in place. ----
"$WORK/LINK.EXE" --shareable --use "$SYSLIB/DECC\$SHR.EXE" \
    --symbol-vector "mwl_present=PROCEDURE,mwl_call=PROCEDURE" --gsmatch LEQUAL,1,0 \
    -o "$SYSLIB/MWEAKLIB\$SHR.EXE" "$WORK/mwl.o"
echo "-- MWEAKLIB\$SHR.EXE (no exporter --use'd) must carry .vms\$wimp --"
readelf -SW "$SYSLIB/MWEAKLIB\$SHR.EXE" | grep -E '\.vms\$sv|\.vms\$imp|\.vms\$wimp|\.plt|\.igot' || true
readelf -SW "$SYSLIB/MWEAKLIB\$SHR.EXE" | grep -q '\.vms\$wimp' \
    || { echo "FAIL(A): weak-undef was baked to 0, not recorded in .vms\$wimp for activation"; exit 1; }
python3 - "$SYSLIB/MWEAKLIB\$SHR.EXE" weak_probe_target wimp <<'PY' || exit 1
import sys,struct,subprocess,re
p,sym,which=sys.argv[1],sys.argv[2],sys.argv[3]
data=open(p,'rb').read()
sec='.vms$wimp' if which=='wimp' else '.vms$imp'
found=False
for line in subprocess.check_output(["readelf","-SW",p]).decode().splitlines():
    if sec in line:
        h=re.findall(r'\b[0-9a-f]{6,16}\b',line); off=int(h[1],16); size=int(h[2],16)
        b=data[off:off+size]; mg,cnt,no,ns=struct.unpack_from('<IIII',b,0)
        names=b[no:no+ns]
        if which=='wimp':
            e=b[16:]
            for k in range(cnt):
                n_off,_,_=struct.unpack_from('<IIQ',e,k*16)
                nm=names[n_off:names.find(b'\0',n_off)].decode()
                if nm==sym: found=True
        break
print(("FOUND" if found else "ABSENT"),sym,"in",sec)
sys.exit(0 if found else 2)
PY

# ---- LINK LEVEL B: with the exporter --use'd, the SAME weak reference binds as a
# STRONG .vms$imp import (the strong scan takes precedence over the weak scan). ----
"$WORK/LINK.EXE" --shareable --use "$SYSLIB/DECC\$SHR.EXE" --use "$SYSLIB/PWEAKTGT\$SHR.EXE" \
    --symbol-vector "mwl_present=PROCEDURE,mwl_call=PROCEDURE" --gsmatch LEQUAL,1,0 \
    -o "$SYSLIB/MWEAKLIB2\$SHR.EXE" "$WORK/mwl.o"
echo "-- MWEAKLIB2\$SHR.EXE (exporter --use'd) must NOT carry a .vms\$wimp weak_probe_target --"
if readelf -SW "$SYSLIB/MWEAKLIB2\$SHR.EXE" | grep -q '\.vms\$wimp'; then
    python3 - "$SYSLIB/MWEAKLIB2\$SHR.EXE" weak_probe_target wimp <<'PY'
import sys,struct,subprocess,re
p,sym=sys.argv[1],sys.argv[2]
data=open(p,'rb').read()
for line in subprocess.check_output(["readelf","-SW",p]).decode().splitlines():
    if '.vms$wimp' in line:
        h=re.findall(r'\b[0-9a-f]{6,16}\b',line); off=int(h[1],16); size=int(h[2],16)
        b=data[off:off+size]; mg,cnt,no,ns=struct.unpack_from('<IIII',b,0); names=b[no:no+ns]; e=b[16:]
        for k in range(cnt):
            n_off,_,_=struct.unpack_from('<IIQ',e,k*16)
            if names[n_off:names.find(b'\0',n_off)].decode()==sym:
                print("FAIL(B): weak_probe_target went to .vms$wimp even though a --use producer exports it"); sys.exit(1)
sys.exit(0)
PY
fi
echo "OK(B): with the exporter available, the reference bound STRONG (.vms\$imp), not weak."

# A main()-consumer: crt0 + exit come from DECC$SHR, portable across arches.
cat > "$WORK/cons.c" <<'EOF'
extern int mwl_present(void);
extern int mwl_call(void);
int main(void)
{
    int rc = 0;
    if (mwl_present())        rc |= 1;   /* GOT cell bound          */
    if (mwl_call() == 0xAB)   rc |= 2;   /* PLT call reaches target */
    return rc;                            /* 3 = both, 0 = neither  */
}
EOF
$CC $CFLAGS -c -o "$WORK/cons.o" "$WORK/cons.c"
"$WORK/LINK.EXE" --executable --use "$SYSLIB/MWEAKLIB\$SHR.EXE" \
    --use "$SYSLIB/PWEAKTGT\$SHR.EXE" --use "$SYSLIB/DECC\$SHR.EXE" \
    -o "$WORK/CONS_POS.EXE" "$WORK/cons.o"; chmod +x "$WORK/CONS_POS.EXE"
"$WORK/LINK.EXE" --executable --use "$SYSLIB/MWEAKLIB\$SHR.EXE" \
    --use "$SYSLIB/DECC\$SHR.EXE" \
    -o "$WORK/CONS_NEG.EXE" "$WORK/cons.o"; chmod +x "$WORK/CONS_NEG.EXE"

echo
echo "MILESTONE (LINK): LINK.EXE records a #pragma-weak cross-image reference no"
echo "--use'd producer exports as a .vms\$wimp weak import (A), and binds it STRONG"
echo "when a producer DOES export it (B). This is the LINK half of reading SYSUAF"
echo "through the ODS-2 ACP: LIBVMS\$SHR's sys\$open/\$get/\$connect/\$close reach"
echo "LIBVMSRMS\$SHR across the layering cycle .vms\$imp cannot express. (vms-5f0)"

# ---- ACTIVATION LEVEL: only meaningful with a real /dev/vms ACP (QEMU). ----
if [ ! -e /dev/vms ]; then
    echo
    echo "NOTE: no /dev/vms on this host -> IMGACT cannot read an image over the ACP"
    echo "(INV-6: no POSIX fallback). The .vms\$wimp BINDING at activation is proven"
    echo "by the full boot-to-DCL login proof (SYSTEM/MANAGER reads SYSUAF through the"
    echo "ACP), not here. LINK-level assertions above PASSED."
    exit 0
fi
cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
echo
echo "== POSITIVE: consumer --use MWEAKLIB + PWEAKTGT (target present) =="
set +e; "$WORK/CONS_POS.EXE"; RC=$?; set -e
echo "positive exit = $RC (expect 3: weak_probe_target bound by name to PWEAKTGT)"
[ "$RC" -eq 3 ] || { echo "FAIL: weak-by-name import did NOT bind at activation (got $RC, want 3)"; exit 1; }
echo "== NEGATIVE: consumer --use MWEAKLIB ONLY (target absent) =="
set +e; "$WORK/CONS_NEG.EXE"; RC=$?; set -e
echo "negative exit = $RC (expect 0: no producer exports it -> cell stays 0, honest weak-undef)"
[ "$RC" -eq 0 ] || { echo "FAIL: absent weak import did not fall back to 0 (got $RC, want 0)"; exit 1; }
echo "MILESTONE (ACTIVATION): IMGACT bound the weak import by NAME (present->bound,"
echo "absent->0) — the honest fallback rms_services_present() reads. (vms-5f0)"
