#!/bin/sh
# run_test_vax.sh — IMGACT.EXE elf32-vax backend build + structural proof
# (rd vms-73b2, epic vms-404). The VAX analogue of run_test_alpha.sh, adapted
# for the one hard constraint the design names (§2 R6): there is NO qemu-system
# VAX and no qemu-user VAX, so this P2 gate CANNOT execute a VAX binary. Runtime
# activation on the SIMH rail is P4 (a separate item). What P2 proves here is:
#
#   1. the elf32-generalized imgact.c + the new src/imgact/arch/vax backend
#      BUILD for vax--netbsdelf and link into a real elf32-vax ET_DYN;
#   2. that binary is a clean freestanding static-PIE interpreter — ELF32 /
#      Digital VAX / DYN, NO PT_INTERP, NO bogus DT_NEEDED, and its own dynamic
#      relocations are R_VAX_RELATIVE with ZERO undefined symbols: the three
#      runtime-libc gaps the P2 backend left open (libgcc-class __udiv/__urem,
#      the __fstat50 fstat rename, and the 64-bit-off_t mmap ABI) are RESOLVED
#      in-image by rd vms-33b, so IMGACT.EXE is a runnable static-PIE (the
#      runtime-activation proof on the SIMH VAX rail is still P4);
#   3. the freestanding VAX syscall primitive (syscall6, arch/vax/start.S) is
#      structurally correct in isolation — it issues `chmk`, tests the carry
#      flag, and returns -errno (the NetBSD/vax syscall convention); and
#   4. the toolchain emits a REAL elf32-vax object carrying a `.vms$sv` symbol
#      vector (the container LINK.EXE will produce in P3), readelf-verified.
#
# rd vms-33b adds assertions 2b/2c/2d/2e below: zero undefined dynamic symbols,
# __udiv/__urem/__fstat50 DEFINED (not UND), the mmap path issues an 8-word
# chmk to SYS_mmap (the pad + 64-bit off_t ABI), and off_t is 64-bit / long is
# 32-bit on this target (the premise the pad word rests on). ABI ground-truthed
# against the sysroot libc's own mmap/fstat stubs, not guessed.
#
# The by-INDEX symbol-vector RESOLVE over a real elf32-vax `.vms$sv` is proven
# by the host ctest imgact_symvec_elf32_unit (src/imgact/test/test_symvec_elf32.c),
# which runs in the normal (non-cross) test job.
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile). Exit 0
# only if every assertion holds; fail honest otherwise (no fabricated pass).

set -eu

PREFIX=${PREFIX:-/opt/cross}
TARGET=vax--netbsdelf
CC="$PREFIX/bin/$TARGET-gcc"
RE="$PREFIX/bin/$TARGET-readelf"
OD="$PREFIX/bin/$TARGET-objdump"
NM="$PREFIX/bin/$TARGET-nm"
SRC=$(cd "$(dirname "$0")/../../.." && pwd)   # repo root
BUILD=${BUILD:-/tmp/imgact-vax}

echo "== toolchain =="
"$CC" --version | head -1

echo "== configure + build IMGACT.EXE for $TARGET (OVMX_IMGACT=ON) =="
rm -rf "$BUILD"; mkdir -p "$BUILD"
cmake -S "$SRC" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$SRC/tools/cross-vax/toolchain-vax-netbsd.cmake" \
    -DOVMX_IMGACT=ON -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF >/dev/null
cmake --build "$BUILD" --target imgact

IMG=$(find "$BUILD" -name IMGACT.EXE | head -1)
[ -n "$IMG" ] && [ -f "$IMG" ] || { echo "FAIL: IMGACT.EXE not built"; exit 1; }
echo "built $IMG ($(stat -c %s "$IMG") bytes)"

echo "== assert: ELF32 / Digital VAX / ET_DYN =="
hdr=$("$RE" -h "$IMG")
echo "$hdr" | grep -q "Class:.*ELF32"        || { echo "FAIL: not ELF32"; exit 1; }
echo "$hdr" | grep -qi "Machine:.*VAX"        || { echo "FAIL: not Digital VAX"; exit 1; }
echo "$hdr" | grep -q "Type:.*DYN"           || { echo "FAIL: not ET_DYN"; exit 1; }
echo "ok: elf32-vax ET_DYN"

echo "== assert: no PT_INTERP (a static-PIE interpreter must not itself have one) =="
if "$RE" -l "$IMG" | grep -qi "interpreter"; then
    echo "FAIL: IMGACT.EXE carries a PT_INTERP"; exit 1
fi
echo "ok: no PT_INTERP"

echo "== assert: no bogus DT_NEEDED (freestanding: no libc/libpthread/libm) =="
if "$RE" -dW "$IMG" 2>/dev/null | grep -qiE "NEEDED"; then
    echo "FAIL: IMGACT.EXE has DT_NEEDED entries:"; "$RE" -dW "$IMG" | grep -i NEEDED
    exit 1
fi
echo "ok: no DT_NEEDED"

echo "== assert: own dynamic relocs are R_VAX_RELATIVE ONLY — no external imports =="
"$RE" -rW "$IMG" 2>/dev/null | grep -oE "R_VAX_[A-Z_0-9]+" | sort | uniq -c
"$RE" -rW "$IMG" 2>/dev/null | grep -q "R_VAX_RELATIVE" || { echo "FAIL: no R_VAX_RELATIVE self-relocs"; exit 1; }
# rd vms-33b: a runnable static-PIE resolves EVERY symbol in-image. No external
# JMP_SLOT/GLOB_DAT/32 reloc may remain — the P2 backend's __udiv/__urem/
# __fstat50 imports are now defined in the image (start.S + the imgact.c alias).
external=$("$RE" -rW "$IMG" 2>/dev/null | grep -E "R_VAX_(JMP_SLOT|GLOB_DAT|32)" || true)
if [ -n "$external" ]; then
    echo "FAIL: IMGACT.EXE still has external relocation(s) — not a runnable static-PIE:"
    echo "$external"; exit 1
fi
echo "ok: relocs are RELATIVE only (no external imports)"

echo "== assert (vms-33b): ZERO undefined dynamic symbols =="
und=$("$RE" --dyn-syms "$IMG" 2>/dev/null | grep -E "\bUND\b" | awk '$8!=""{print $8}' || true)
if [ -n "$und" ]; then
    echo "FAIL: IMGACT.EXE has undefined dynamic symbol(s):"; echo "$und"; exit 1
fi
# nm is the second witness: no 'U' (undefined) entries at all.
if "$NM" "$IMG" 2>/dev/null | grep -qE "^ *U | U [^ ]"; then
    echo "FAIL: nm reports undefined symbols:"; "$NM" "$IMG" 2>/dev/null | grep " U " ; exit 1
fi
echo "ok: no undefined symbols (readelf --dyn-syms + nm agree)"

echo "== assert (vms-33b): __udiv/__urem/__fstat50 are DEFINED (T/t), not UND =="
for s in __udiv __urem __fstat50; do
    "$NM" "$IMG" 2>/dev/null | grep -E "^[0-9a-f]+ [Tt] $s\$" >/dev/null \
        || { echo "FAIL: $s is not a defined text symbol"; "$NM" "$IMG" 2>/dev/null | grep "$s" || echo "(absent)"; exit 1; }
done
echo "ok: __udiv/__urem/__fstat50 defined in-image"

echo "== assert (vms-33b): mmap path is the 8-word NetBSD/vax ABI (pad + 64-bit off_t) =="
# The VAX backend must NOT route mmap through the 6-arg syscall6 (which would
# leave the kernel reading off_lo/off_hi from uninitialized stack). It uses a
# dedicated imgact_vax_mmap that builds an 8-word arg list { addr,len,prot,
# flags,fd,pad,off_lo,off_hi } and chmk's SYS_mmap (197 = 0xc5).
mm=$("$OD" -d "$IMG" | awk '/<imgact_vax_mmap>:/{f=1} f{print} f&&/ret$/{c++} c>=2{exit}')
[ -n "$mm" ] || { echo "FAIL: imgact_vax_mmap not present"; exit 1; }
echo "$mm" | grep -qi "chmk"                 || { echo "FAIL: imgact_vax_mmap has no chmk"; echo "$mm"; exit 1; }
echo "$mm" | grep -qiE "movl +\\\$0x8," || echo "$mm" | grep -qi "pushl \$0x8" \
    || { echo "FAIL: imgact_vax_mmap does not push an 8-word arg count"; echo "$mm"; exit 1; }
# SYS_mmap immediate (197 = 0xc5) loaded into r0 before the chmk.
echo "$mm" | grep -qiE "0x0*c5|movl +\\\$0xc5" \
    || { echo "FAIL: imgact_vax_mmap does not chmk SYS_mmap (197/0xc5)"; echo "$mm"; exit 1; }
echo "ok: imgact_vax_mmap issues an 8-word chmk to SYS_mmap"

echo "== assert (vms-33b): the mmap-ABI premise holds against the sysroot headers =="
# The pad word + split offset exist BECAUSE off_t is 64-bit and a syscall word
# is 32-bit on VAX. Prove that premise at compile time against the real headers
# (the same sysroot the image links to), rather than assuming it.
cat > "$BUILD/off_abi.c" <<'EOF'
#include <sys/types.h>
_Static_assert(sizeof(off_t) == 8, "NetBSD/vax off_t must be 64-bit (mmap pad+split ABI)");
_Static_assert(sizeof(long)  == 4, "VAX syscall word must be 32-bit");
int off_abi_ok;
EOF
"$CC" -c "$BUILD/off_abi.c" -o "$BUILD/off_abi.o" \
    || { echo "FAIL: off_t/long width _Static_assert did not hold on this target"; exit 1; }
echo "ok: off_t == 8 bytes, long == 4 bytes (mmap pad + 64-bit-offset ABI grounded)"

echo "== assert: syscall6 issues chmk, tests carry, returns -errno (NetBSD/vax ABI) =="
dis=$("$OD" -d "$IMG" | awk '/<syscall6>:/{f=1} f{print} /ret/&&f&&NR>1{n++} n>=2&&f{exit}')
echo "$dis" | grep -qi "chmk"   || { echo "FAIL: syscall6 has no chmk"; echo "$dis"; exit 1; }
echo "$dis" | grep -qiE "blssu|bcs" || { echo "FAIL: syscall6 does not test the carry flag"; echo "$dis"; exit 1; }
echo "$dis" | grep -qi "mnegl" || { echo "FAIL: syscall6 does not negate errno"; echo "$dis"; exit 1; }
echo "ok: syscall6 chmk/carry/-errno shape verified"

echo "== assert: toolchain emits a REAL elf32-vax object carrying a .vms\$sv =="
FX="$BUILD/fixture.s"
cat > "$FX" <<'EOF'
	.section .vms$sv,"a",@progbits
	.long 0x31565356      /* OVMX_SV_MAGIC "VSV1" */
	.long 1               /* count            */
	.long 0               /* gsmatch_kind=ALWAYS */
	.long 0               /* gsmatch_major    */
	.long 0               /* gsmatch_minor    */
	.long 32              /* names_off        */
	.long 6               /* names_size       */
	.long 0               /* reserved         */
	.quad 0x100           /* entry[0].value   */
	.long 1               /* entry[0].kind = PROCEDURE */
	.long 0               /* entry[0].name_off */
	.ascii "myadd\0"      /* names            */
EOF
"$CC" -shared -nostdlib -o "$BUILD/FIXTURE.SO" "$FX"
"$RE" -SW "$BUILD/FIXTURE.SO" | grep -q '\.vms\$sv' || { echo "FAIL: .vms\$sv not emitted"; exit 1; }
"$RE" -h "$BUILD/FIXTURE.SO" | grep -qi "Machine:.*VAX" || { echo "FAIL: fixture not elf32-vax"; exit 1; }
echo "ok: cross toolchain produced an elf32-vax ET_DYN with a .vms\$sv section"

echo
echo "PASS: elf32-vax IMGACT.EXE builds + is a RUNNABLE static-PIE — zero undefined"
echo "      symbols (__udiv/__urem/__fstat50 resolved in-image), 8-word mmap ABI,"
echo "      syscall6 ABI shape verified, toolchain emits elf32-vax .vms\$sv."
echo "      (Runtime activation on the SIMH VAX rail is P4.)"
