#!/bin/sh
# run_vmsfs_native.sh — fourth real OVMX-library migration onto the VMS-native
# toolchain (bead vms-b65.4, pillar vms-ade). Proves that the actual src/vmsfs
# VMS-filesystem library links VMS-native into LIBVMSFS$SHR.EXE, activates through
# IMGACT.EXE, and a consumer that imports its universals gets the VMS-correct
# result — with NO ld / NO ld.so.
#
# vmsfs is the FIRST b65 lib with TWO OVMX producer dependencies:
#   - it imports libc/ctype/dirent/stat from DECC$SHR (memcpy/memset, str*/
#     strn*case*cmp, snprintf, atoi, qsort, isalnum/tolower/toupper,
#     __errno_location, opendir/readdir/closedir, stat, realpath, open/close/
#     unlink, pthread_mutex_lock/unlock). __errno_location, isalnum, tolower,
#     strncasecmp, opendir/readdir/closedir, stat, realpath, unlink were APPENDED
#     to DECC$SHR's vector for this migration (mk_decc_shr.sh, append-only).
#   - it imports lnm_get_manager + lnm_translate from LIBVMSLNM$SHR (device→path
#     translation consults the logical-name manager) — the FIRST inter-OVMX-lib
#     import bind (vmsfs -> vmslnm), on top of the transitive DECC$SHR bind.
#   - it defines NO __thread objects, so LIBVMSFS$SHR.EXE is NOT a TLS producer
#     (no PT_TLS / .tbss / .vms$tls) — like LIBVMSLNM$SHR (vms-b65.3).
#
# Chain, all VMS-native (no ld / no ld.so):
#   1. build IMGACT.EXE + LINK.EXE.
#   2. mk_decc_shr.sh whole-archives musl libc.a + libgcc.a into DECC$SHR.EXE.
#   3. mk_vmslnm_shr.sh links LIBVMSLNM$SHR.EXE (vmsfs's OVMX dependency).
#   4. mk_vmsfs_shr.sh compiles the 5 real vmsfs library objects (vmsfs_translate,
#      vmsfs_version, vmsfs_case, vmsfs_protect, vmsfs_device) and links
#      LIBVMSFS$SHR.EXE via LINK.EXE --shareable --use DECC$SHR --use LIBVMSLNM$SHR:
#      EXPORTS the filesystem universals, IMPORTS libc FROM DECC$SHR and lnm_* FROM
#      LIBVMSLNM$SHR. STRICT link: every import MUST bind (no --allow-undefined).
#   5. LINK.EXE --executable --use LIBVMSFS$SHR builds a consumer that imports ONLY
#      one vmsfs universal (vmsfs_to_linux_path) — it never names DECC$SHR or
#      LIBVMSLNM$SHR, so BOTH binds are purely TRANSITIVE (IMGACT pulls them from
#      LIBVMSFS$SHR's .vms$imp).
#   6. RUN the consumer FOR REAL: kernel -> IMGACT.EXE -> load LIBVMSFS$SHR ->
#      (transitively) load DECC$SHR + LIBVMSLNM$SHR, bind vmsfs's libc + lnm_*
#      imports -> bind the consumer's vmsfs_to_linux_path import -> drive musl
#      __init_libc -> transfer control. The consumer:
#        vmsfs_to_linux_path("[MYDIR]DATA.TXT", buf, ...)
#          -> SS$_NORMAL (1); buf == "/vms/MYDIR/data.txt".
#      VMS translation semantics proven exactly: no device -> system-disk root
#      /vms (SYSDISK_MOUNT); [MYDIR] -> /MYDIR (dir case preserved); DATA.TXT ->
#      data.txt (file name+type lowercased for the Linux fs). Consumer exits 42
#      iff (rc == SS$_NORMAL && buf == "/vms/MYDIR/data.txt"). Exit 42 proves the
#      vmsfs universal bound in the consumer, the DECC$SHR C-RTL + LIBVMSLNM$SHR
#      logical-name producers bound transitively and ran, and VMS filespec->Linux
#      translation is exactly correct.
#
# Uses the arm64 musl container's libc.a + libgcc.a (aarch64-only for now,
# CLAUDE.md test loop). Needs root to create /vms. Exit 0 on ok.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
VMSLNM_DIR=$(cd "$IMGACT_DIR/../vmslnm" && pwd)
LIBVMSSYS_DIR=$(cd "$IMGACT_DIR/../libvmssys" && pwd)   # vms_kif_lnm_* producer (vms-96e2)
VMSFS_DIR=$(cd "$IMGACT_DIR/../vmsfs" && pwd)
LIBVMS_INC=$(cd "$IMGACT_DIR/../libvms/include" && pwd)
LNM_INC=$(cd "$IMGACT_DIR/../vmslnm/include" && pwd)
WORK=${WORK:-/tmp/vmsfs-native}
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

# LIBVMSSYS$SHR: vmslnm's LNM$SYSTEM path imports vms_kif_lnm_* from it (vms-96e2),
# so it must exist next to DECC$SHR for the STRICT vmslnm link and for IMGACT's
# transitive resolution.
echo "== mk_vmssys_shr.sh: real src/libvmssys -> LIBVMSSYS\$SHR.EXE (vms_kif_* producer) =="
CC="$CC" sh "$LINK_DIR/mk_vmssys_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSSYS\$SHR.EXE" "$LIBVMSSYS_DIR"
readelf -SW "$SYSLIB/LIBVMSSYS\$SHR.EXE" | grep -q '\.vms\$sv' || { echo "FAIL: LIBVMSSYS\$SHR has no symbol vector"; exit 1; }

echo "== mk_vmslnm_shr.sh: real src/vmslnm -> LIBVMSLNM\$SHR.EXE (vmsfs's OVMX dep) =="
CC="$CC" sh "$LINK_DIR/mk_vmslnm_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$VMSLNM_DIR" "$LIBVMS_INC"

echo "== mk_vmsfs_shr.sh: real src/vmsfs -> LIBVMSFS\$SHR.EXE =="
# STRICT link inside the recipe (no --allow-undefined). If this fails on an
# unresolved libc symbol, DECC$SHR's vector needs that universal appended; on an
# unresolved lnm_* symbol, LIBVMSLNM$SHR's vector needs it.
CC="$CC" sh "$LINK_DIR/mk_vmsfs_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSFS\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" \
    "$VMSFS_DIR" "$LIBVMS_INC" "$LNM_INC"

echo "-- LIBVMSFS\$SHR.EXE: symbol vector + bound imports; NO TLS (vmsfs has no __thread) --"
readelf -SW "$SYSLIB/LIBVMSFS\$SHR.EXE" | grep -E '\.vms\$sv|\.vms\$imp|\.plt|\.igot' || true
readelf -SW "$SYSLIB/LIBVMSFS\$SHR.EXE" | grep -q '\.vms\$sv'  || { echo "FAIL: no .vms\$sv (no universals)"; exit 1; }
readelf -SW "$SYSLIB/LIBVMSFS\$SHR.EXE" | grep -q '\.vms\$imp' || { echo "FAIL: no .vms\$imp (imports not bound)"; exit 1; }
# vmsfs is NOT a TLS producer: assert the negative so a future accidental __thread
# (which would silently need the multi-object-TLS path, vms-212) is caught here.
if readelf -lW "$SYSLIB/LIBVMSFS\$SHR.EXE" | grep -q '\bTLS\b'; then
    echo "FAIL: LIBVMSFS\$SHR unexpectedly has PT_TLS (vmsfs gained a __thread object?)"; exit 1
fi

echo "== LINK.EXE --executable --use LIBVMSFS\$SHR -> consumer =="
# Imports ONLY the vmsfs_to_linux_path universal. The DECC$SHR + LIBVMSLNM$SHR
# binds are purely transitive (IMGACT must pull them from LIBVMSFS$SHR's .vms$imp).
cat > "$WORK/cons.c" <<'EOF'
extern int vmsfs_to_linux_path(const char *vms_spec, char *linux_path,
                               unsigned long path_size);

/* Build strings byte-by-byte on the stack: string LITERALS would land in the
 * consumer's .rodata and be reached via ADRP/ADD (PC-relative) relocs, which the
 * VMS-native emit_executable does NOT support (only CALL26/JUMP26 + GOT). Explicit
 * char stores compile to immediate STRB — no relocation. (vms-338) */
void _start(void)
{
    char spec[16];                          /* "[MYDIR]DATA.TXT" */
    spec[0]='['; spec[1]='M'; spec[2]='Y'; spec[3]='D'; spec[4]='I'; spec[5]='R';
    spec[6]=']'; spec[7]='D'; spec[8]='A'; spec[9]='T'; spec[10]='A'; spec[11]='.';
    spec[12]='T'; spec[13]='X'; spec[14]='T'; spec[15]=0;

    char want[20];                          /* "/vms/MYDIR/data.txt" */
    want[0]='/'; want[1]='v'; want[2]='m'; want[3]='s'; want[4]='/';
    want[5]='M'; want[6]='Y'; want[7]='D'; want[8]='I'; want[9]='R';
    want[10]='/'; want[11]='d'; want[12]='a'; want[13]='t'; want[14]='a';
    want[15]='.'; want[16]='t'; want[17]='x'; want[18]='t'; want[19]=0;

    char buf[256];
    for (int i = 0; i < 256; i++) buf[i] = (char)0xAB;   /* poison */

    int rc = vmsfs_to_linux_path(spec, buf, sizeof(buf)); /* SS$_NORMAL = 1 */

    /* byte-exact compare against the expected VMS->Linux translation */
    int match = 1;
    for (int i = 0; i < 20; i++) { if (buf[i] != want[i]) { match = 0; break; } }

    int code = (rc == 1 && match) ? 42 : 1;

    register long x8 __asm__("x8") = 94;   /* exit_group */
    register long x0 __asm__("x0") = code;
    __asm__ volatile("svc 0" :: "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}
EOF
# -O0: at -O2 gcc coalesces the byte stores into a .rodata constant pool reached
# via ADRP/LDST_ABS relocs, which the VMS-native emit_executable rejects (it
# supports only CALL26/JUMP26 + GOT in .text). -O0 leaves the arrays as plain STRB
# immediates, so the object carries ONLY the CALL26 reloc (vmsfs_to_linux_path).
$CC -fPIC -O0 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics \
    -c -o "$WORK/cons.o" "$WORK/cons.c"
"$WORK/LINK.EXE" --executable --use "$SYSLIB/LIBVMSFS\$SHR.EXE" \
    -o "$WORK/CONS.EXE" "$WORK/cons.o"
chmod +x "$WORK/CONS.EXE"

echo
echo "== RUN ./CONS.EXE FOR REAL (kernel -> IMGACT -> LIBVMSFS\$SHR -> DECC\$SHR + LIBVMSLNM\$SHR) =="
set +e
"$WORK/CONS.EXE"; RC=$?
set -e
echo "exit code = $RC (expect 42 = vmsfs_to_linux_path==SS\$_NORMAL(1) && result==\"/vms/MYDIR/data.txt\")"
[ "$RC" -eq 42 ] || { echo "FAIL: vmsfs VMS-native migration did not run correctly (got $RC, want 42)"; exit 1; }

echo
echo "MILESTONE (vms-b65.4): the REAL src/vmsfs VMS-filesystem library links"
echo "VMS-native into LIBVMSFS\$SHR.EXE (its libc imports bound to DECC\$SHR, its"
echo "logical-name imports bound to LIBVMSLNM\$SHR, no TLS), activates through"
echo "IMGACT.EXE, and a consumer gets the VMS-correct filespec->Linux translation."
echo "Fourth link in the b65 chain; unblocks vms-b65.2 (libvms)."
