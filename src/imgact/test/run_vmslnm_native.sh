#!/bin/sh
# run_vmslnm_native.sh — third real OVMX-library migration onto the VMS-native
# toolchain (bead vms-b65.3, pillar vms-ade). Proves that the actual src/vmslnm
# logical-name manager links VMS-native into LIBVMSLNM$SHR.EXE, activates through
# IMGACT.EXE, and a consumer that imports its universals gets the VMS-correct
# result — with NO ld / NO ld.so.
#
# vmslnm is a NEAR-LEAF and the SIMPLEST migration so far:
#   - it depends only on pthread (from DECC$SHR); NOT on vmsprocess/vmsfs/libvms.
#   - it defines NO __thread objects, so LIBVMSLNM$SHR.EXE is NOT a TLS producer
#     (no PT_TLS / .tbss / .vms$tls) — unlike LIBVMSPROCESS$SHR (vms-b65.1).
#   - it imports only libc/pthread from DECC$SHR (calloc/free/memcpy, strlen/
#     strncpy/strchr/strcasecmp/toupper, pthread_once, ttyname). pthread_once,
#     toupper, and ttyname were appended to DECC$SHR's vector for this migration.
#
# Chain, all VMS-native (no ld / no ld.so):
#   1. build IMGACT.EXE + LINK.EXE.
#   2. mk_decc_shr.sh whole-archives musl libc.a + libgcc.a into DECC$SHR.EXE.
#   3. mk_vmslnm_shr.sh compiles the 4 real vmslnm library objects (lnm_table,
#      lnm_translate, lnm_client, lnm_defaults) and links LIBVMSLNM$SHR.EXE via
#      LINK.EXE --shareable --use DECC$SHR: EXPORTS the logical-name universals
#      (lnm_init/get_manager/create/translate/...), IMPORTS libc/pthread FROM
#      DECC$SHR. STRICT link: every import MUST bind (no --allow-undefined).
#   4. LINK.EXE --executable --use LIBVMSLNM$SHR builds a consumer that imports
#      ONLY vmslnm universals (lnm_get_manager + lnm_create + lnm_translate) — it
#      never names DECC$SHR, so the DECC$SHR bind is purely TRANSITIVE.
#   5. RUN the consumer FOR REAL: kernel -> IMGACT.EXE -> load LIBVMSLNM$SHR ->
#      (transitively) load DECC$SHR, bind vmslnm's pthread/calloc imports -> bind
#      the consumer's lnm_* imports -> drive musl __init_libc -> transfer control.
#      The consumer:
#        lnm_get_manager()  -> pthread_once(DECC$SHR) drives lnm_init: calloc the
#                              manager + 4 tables (LNM$PROCESS/JOB/GROUP/SYSTEM).
#        lnm_create(mgr, "LNM$PROCESS_TABLE", "FOO$OVMX", "BAR_VALUE", 0, 3)
#                           -> SS$_NORMAL (1): insert the logical.
#        lnm_translate(mgr, "LNM$PROCESS_TABLE", "FOO$OVMX", buf, ...)
#                           -> SS$_NORMAL (1); buf == "BAR_VALUE", len == 9.
#      Consumer exits 42 iff (mgr!=NULL && rc1==1 && rc2==1 && len==9 &&
#      buf=="BAR_VALUE"). Exit 42 proves the vmslnm universals bound in the
#      consumer, the pthread_once/calloc path bound transitively to DECC$SHR and
#      ran, and VMS logical-name create+translate semantics are exactly correct.
#
# Uses the arm64 musl container's libc.a + libgcc.a (aarch64-only for now,
# CLAUDE.md test loop). Needs root to create /vms. Exit 0 on ok.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
VMSLNM_DIR=$(cd "$IMGACT_DIR/../vmslnm" && pwd)
LIBVMS_INC=$(cd "$IMGACT_DIR/../libvms/include" && pwd)
WORK=${WORK:-/tmp/vmslnm-native}
rm -rf "$WORK"; mkdir -p "$WORK"

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

LIBC=${LIBC:-/usr/lib/libc.a}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]   || { echo "SKIP-FAIL: no musl libc.a at $LIBC (need arm64 musl container)"; exit 1; }
[ -f "$LIBGCC" ] || { echo "FAIL: no libgcc.a at $LIBGCC"; exit 1; }

echo "== build IMGACT.EXE + LINK.EXE =="
( cd "$IMGACT_DIR" && make CC="$CC" clean >/dev/null 2>&1 || true; make CC="$CC" ) >/dev/null 2>&1
cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -o "$WORK/LINK.EXE" "$LINK_DIR/link.c"

echo "== mk_decc_shr.sh: whole-archive musl -> DECC\$SHR.EXE (production vector) =="
sh "$LINK_DIR/mk_decc_shr.sh" "$WORK/LINK.EXE" "$SYSLIB/DECC\$SHR.EXE" "$LIBC" "$LIBGCC"
readelf -SW "$SYSLIB/DECC\$SHR.EXE" | grep -q '\.vms\$sv' || { echo "FAIL: DECC\$SHR has no symbol vector"; exit 1; }

echo "== mk_vmslnm_shr.sh: real src/vmslnm -> LIBVMSLNM\$SHR.EXE =="
# STRICT link inside the recipe (no --allow-undefined). If this fails on an
# unresolved libc symbol, DECC$SHR's vector needs that universal appended.
CC="$CC" sh "$LINK_DIR/mk_vmslnm_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$VMSLNM_DIR" "$LIBVMS_INC"

echo "-- LIBVMSLNM\$SHR.EXE: symbol vector + bound imports; NO TLS (vmslnm has no __thread) --"
readelf -SW "$SYSLIB/LIBVMSLNM\$SHR.EXE" | grep -E '\.vms\$sv|\.vms\$imp|\.plt|\.igot' || true
readelf -SW "$SYSLIB/LIBVMSLNM\$SHR.EXE" | grep -q '\.vms\$sv'  || { echo "FAIL: no .vms\$sv (no universals)"; exit 1; }
readelf -SW "$SYSLIB/LIBVMSLNM\$SHR.EXE" | grep -q '\.vms\$imp' || { echo "FAIL: no .vms\$imp (imports not bound to DECC\$SHR)"; exit 1; }
# vmslnm is NOT a TLS producer: assert the negative so a future accidental __thread
# (which would silently need the multi-object-TLS path, vms-212) is caught here.
if readelf -lW "$SYSLIB/LIBVMSLNM\$SHR.EXE" | grep -q '\bTLS\b'; then
    echo "FAIL: LIBVMSLNM\$SHR unexpectedly has PT_TLS (vmslnm gained a __thread object?)"; exit 1
fi

echo "== LINK.EXE --executable --use LIBVMSLNM\$SHR -> consumer =="
# Imports ONLY vmslnm universals. The DECC$SHR bind is purely transitive (IMGACT
# must pull it from LIBVMSLNM$SHR's .vms$imp).
cat > "$WORK/cons.c" <<'EOF'
extern void *lnm_get_manager(void);
extern unsigned int lnm_create(void *mgr, const char *table, const char *name,
                               const char *equiv, unsigned int attr,
                               unsigned char acmode);
extern unsigned int lnm_translate(void *mgr, const char *table, const char *name,
                                  char *result, unsigned long result_size,
                                  unsigned short *result_length,
                                  unsigned int *attributes);

/* Build strings byte-by-byte on the stack: string LITERALS would land in the
 * consumer's .rodata and be reached via ADRP/ADD (PC-relative) relocs, which the
 * VMS-native emit_executable does NOT support (only CALL26/JUMP26 + GOT). Explicit
 * char stores compile to immediate STRB — no relocation. */
void _start(void)
{
    char tbl[18];                            /* "LNM$PROCESS_TABLE" */
    tbl[0]='L'; tbl[1]='N'; tbl[2]='M'; tbl[3]='$'; tbl[4]='P'; tbl[5]='R';
    tbl[6]='O'; tbl[7]='C'; tbl[8]='E'; tbl[9]='S'; tbl[10]='S'; tbl[11]='_';
    tbl[12]='T'; tbl[13]='A'; tbl[14]='B'; tbl[15]='L'; tbl[16]='E'; tbl[17]=0;

    char nam[9];                             /* "FOO$OVMX" */
    nam[0]='F'; nam[1]='O'; nam[2]='O'; nam[3]='$';
    nam[4]='O'; nam[5]='V'; nam[6]='M'; nam[7]='X'; nam[8]=0;

    char eqv[10];                            /* "BAR_VALUE" */
    eqv[0]='B'; eqv[1]='A'; eqv[2]='R'; eqv[3]='_'; eqv[4]='V';
    eqv[5]='A'; eqv[6]='L'; eqv[7]='U'; eqv[8]='E'; eqv[9]=0;

    void *mgr = lnm_get_manager();          /* pthread_once(DECC$SHR) -> lnm_init */
    char buf[64];
    unsigned short len = 0;
    unsigned int rc1 = lnm_create(mgr, tbl, nam, eqv, 0, 3);   /* SS$_NORMAL = 1 */
    unsigned int rc2 = lnm_translate(mgr, tbl, nam,
                                     buf, sizeof(buf), &len, 0); /* SS$_NORMAL = 1 */

    /* "BAR_VALUE" is 9 bytes; verify first/last char + NUL + length */
    int val_ok = (len == 9 && buf[0] == 'B' && buf[8] == 'E' && buf[9] == '\0');
    int code = (mgr != 0 && rc1 == 1 && rc2 == 1 && val_ok) ? 42 : 1;

    register long x8 __asm__("x8") = 94;   /* exit_group */
    register long x0 __asm__("x0") = code;
    __asm__ volatile("svc 0" :: "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}
EOF
# -O0: at -O2 gcc coalesces the byte stores into a .rodata constant pool reached via
# ADRP/LDST_ABS relocs, which the VMS-native emit_executable rejects (it supports
# only CALL26/JUMP26 + GOT in .text). -O0 leaves the array as plain STRB immediates,
# so the object carries ONLY the 3 CALL26 relocs (lnm_get_manager/create/translate).
$CC -fPIC -O0 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics \
    -c -o "$WORK/cons.o" "$WORK/cons.c"
"$WORK/LINK.EXE" --executable --use "$SYSLIB/LIBVMSLNM\$SHR.EXE" \
    -o "$WORK/CONS.EXE" "$WORK/cons.o"
chmod +x "$WORK/CONS.EXE"

echo
echo "== RUN ./CONS.EXE FOR REAL (kernel -> IMGACT -> LIBVMSLNM\$SHR -> DECC\$SHR) =="
set +e
"$WORK/CONS.EXE"; RC=$?
set -e
echo "exit code = $RC (expect 42 = mgr!=NULL && lnm_create==SS\$_NORMAL(1) && lnm_translate==SS\$_NORMAL(1) && value==\"BAR_VALUE\")"
[ "$RC" -eq 42 ] || { echo "FAIL: vmslnm VMS-native migration did not run correctly (got $RC, want 42)"; exit 1; }

echo
echo "MILESTONE (vms-b65.3): the REAL src/vmslnm logical-name manager links"
echo "VMS-native into LIBVMSLNM\$SHR.EXE (its libc/pthread imports bound to DECC\$SHR,"
echo "no TLS), activates through IMGACT.EXE, and a consumer gets VMS-correct"
echo "logical-name create+translate results. Third link in the b65 chain; unblocks"
echo "vms-b65.4 (vmsfs)."
