#!/bin/sh
# run_exe_weak_import_activation.sh — EXECUTABLE-side .vms$wimp binding (vms-4a6).
#
# Companion to run_weak_import_activation.sh, which proves a PRODUCER shareable's
# weak-by-name imports bind at activation. That path (g_prods[]) already worked.
# This proves the SYMMETRIC case the activator was silently dropping: the MAIN
# EXECUTABLE's OWN .vms$wimp. A whole-archived C++ image (libstdc++/libgcc pulled
# flat into IMAGE.EXE) weak-references a universal that NO --use'd producer
# exports at link time but a producer loaded TRANSITIVELY at activation does. On
# the pre-fix activator resolve_weak_imports() iterated only g_prods[]; g_exe was
# never in it, so an executable's resolvable weak import was left 0 -> its first
# call jumps to address 0 (the CPPTEST/cc1 first-light crash class). The fix
# resolves g_exe.wimp against the same producer set.
#
# Topology (exporter reached only TRANSITIVELY, so the exe cannot --use it and the
# reference is therefore WEAK, yet the producer IS loaded at activation):
#   PWEAKTGT$SHR   exports weak_probe_target()  (the higher-layer exporter)
#   BRIDGE$SHR     --use PWEAKTGT; exports bridge_touch() that STRONG-calls
#                  weak_probe_target -> PWEAKTGT lands in BRIDGE's .vms$imp, so
#                  loading BRIDGE transitively loads PWEAKTGT into g_prods[].
#   CONS.EXE       #pragma weak weak_probe_target; --use BRIDGE + DECC ONLY.
#                  weak_probe_target is exported by NO --use'd producer -> LINK
#                  records it in the EXECUTABLE's .vms$wimp. main() strong-calls
#                  bridge_touch() (pulls BRIDGE+PWEAKTGT resident), then binds the
#                  weak cell and calls through it: 5 = bound, 0 = left unbound.
#
# Host musl (x86_64 or aarch64). Needs write to /vms. Exit 0 on ok.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)
IMGACT_DIR=$(cd "$HERE/.." && pwd)
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
WORK=${WORK:-/tmp/exe-weak-import-act}
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

echo "== build IMGACT.EXE + LINK.EXE ($ARCH) =="
( cd "$IMGACT_DIR" && make CC="$CC" ARCH="$ARCH" clean >/dev/null 2>&1 || true; make CC="$CC" ARCH="$ARCH" ) >/dev/null 2>&1
cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -o "$WORK/LINK.EXE" "$LINK_DIR/link.c"

echo "== mk_decc_shr.sh -> DECC\$SHR.EXE =="
sh "$LINK_DIR/mk_decc_shr.sh" "$WORK/LINK.EXE" "$SYSLIB/DECC\$SHR.EXE" "$LIBC" "$LIBGCC" >/dev/null

echo "== PWEAKTGT\$SHR.EXE: exports weak_probe_target() =="
cat > "$WORK/ptgt.c" <<'EOF'
int weak_probe_target(void) { return 0xAB; }
EOF
$CC $CFLAGS -c -o "$WORK/ptgt.o" "$WORK/ptgt.c"
"$WORK/LINK.EXE" --shareable --use "$SYSLIB/DECC\$SHR.EXE" \
    --symbol-vector "weak_probe_target=PROCEDURE" --gsmatch LEQUAL,1,0 \
    -o "$SYSLIB/PWEAKTGT\$SHR.EXE" "$WORK/ptgt.o"

echo "== BRIDGE\$SHR.EXE: --use PWEAKTGT; STRONG-calls weak_probe_target (pulls it resident) =="
cat > "$WORK/bridge.c" <<'EOF'
extern int weak_probe_target(void);          /* STRONG: BRIDGE --use's PWEAKTGT */
int bridge_touch(void) { return weak_probe_target(); }
EOF
$CC $CFLAGS -c -o "$WORK/bridge.o" "$WORK/bridge.c"
"$WORK/LINK.EXE" --shareable --use "$SYSLIB/PWEAKTGT\$SHR.EXE" --use "$SYSLIB/DECC\$SHR.EXE" \
    --symbol-vector "bridge_touch=PROCEDURE" --gsmatch LEQUAL,1,0 \
    -o "$SYSLIB/BRIDGE\$SHR.EXE" "$WORK/bridge.o"

echo "== CONS.EXE: #pragma weak weak_probe_target; --use BRIDGE + DECC ONLY =="
cat > "$WORK/cons.c" <<'EOF'
#pragma weak weak_probe_target
extern int weak_probe_target(void);   /* WEAK: no --use'd producer exports it   */
extern int bridge_touch(void);        /* STRONG import: pulls BRIDGE+PWEAKTGT in */
int main(void)
{
    int rc = 0;
    (void)bridge_touch();                  /* ensure PWEAKTGT is resident         */
    if (&weak_probe_target != 0 &&         /* read the exe's weak import GOT cell  */
        weak_probe_target() == 0xAB)
        rc = 5;                            /* 5 = exe weak import bound by name    */
    return rc;                             /* 0 = cell left unbound (pre-fix bug)  */
}
EOF
$CC $CFLAGS -c -o "$WORK/cons.o" "$WORK/cons.c"

# ---- LINK LEVEL: the weak ref must land in the EXECUTABLE's OWN .vms$wimp. ----
"$WORK/LINK.EXE" --executable --use "$SYSLIB/BRIDGE\$SHR.EXE" --use "$SYSLIB/DECC\$SHR.EXE" \
    -o "$SYSEXE/CONS.EXE" "$WORK/cons.o"; chmod +x "$SYSEXE/CONS.EXE"
echo "-- CONS.EXE must carry a .vms\$wimp naming weak_probe_target --"
readelf -SW "$SYSEXE/CONS.EXE" | grep -E '\.vms\$sv|\.vms\$imp|\.vms\$wimp|\.plt|\.igot' || true
python3 - "$SYSEXE/CONS.EXE" weak_probe_target <<'PY' || { echo "FAIL(LINK): weak_probe_target not in the executable's .vms\$wimp"; exit 1; }
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
                print("FOUND",sym,"in executable .vms$wimp"); sys.exit(0)
sys.exit(2)
PY

echo
echo "MILESTONE (LINK): LINK.EXE records the executable's own #pragma-weak"
echo "cross-image reference in IMAGE.EXE's .vms\$wimp (vms-4a6)."

# ---- ACTIVATION LEVEL: bind the executable's OWN weak import by name. The image
# read defers to POSIX open on the /vms path when /dev/vms is absent (imgsrc_open
# SS$_NOSUCHDEV defer), so this runs in the plain build container too. ----
echo
echo "== ACTIVATION: run CONS.EXE (exe weak import resolvable via transitively-loaded PWEAKTGT) =="
set +e; "$SYSEXE/CONS.EXE"; RC=$?; set -e
echo "exit = $RC (expect 5: the EXECUTABLE's own weak import bound by name to PWEAKTGT)"
[ "$RC" -eq 5 ] || { echo "FAIL: executable .vms\$wimp was NOT resolved at activation (got $RC, want 5) — resolve_weak_imports skipped g_exe"; exit 1; }

# ---- FAIL-HONEST: an executable weak import NO loaded producer exports must stay
# 0 (weak-undef), NEVER bind to something wrong. CONS_NEG --use's DECC ONLY, so
# PWEAKTGT is never loaded; the guarded call must be skipped and the image exits
# 0 cleanly (not a crash, not a spurious bind). ----
echo "== FAIL-HONEST: CONS_NEG.EXE (weak import, NO exporter loaded) =="
cat > "$WORK/cons_neg.c" <<'EOF'
#pragma weak weak_probe_target
extern int weak_probe_target(void);   /* WEAK, and no producer exports it */
int main(void)
{
    if (&weak_probe_target != 0)       /* cell must be 0 -> guard false     */
        return weak_probe_target();    /* never taken; if bound-wrong, != 0 */
    return 0;                          /* honest weak-undef fallback         */
}
EOF
$CC $CFLAGS -c -o "$WORK/cons_neg.o" "$WORK/cons_neg.c"
"$WORK/LINK.EXE" --executable --use "$SYSLIB/DECC\$SHR.EXE" \
    -o "$SYSEXE/CONS_NEG.EXE" "$WORK/cons_neg.o"; chmod +x "$SYSEXE/CONS_NEG.EXE"
set +e; "$SYSEXE/CONS_NEG.EXE"; RCN=$?; set -e
echo "exit = $RCN (expect 0: no producer exports it -> cell stays 0, honest weak-undef)"
[ "$RCN" -eq 0 ] || { echo "FAIL: absent executable weak import did not stay 0 (got $RCN) — bound to something wrong"; exit 1; }

echo "MILESTONE (ACTIVATION): IMGACT resolved the MAIN EXECUTABLE's .vms\$wimp"
echo "against the loaded producer set (present -> bound; absent -> 0, fail-honest)."
echo "The g_exe weak-import path is no longer dropped. (vms-4a6)"
