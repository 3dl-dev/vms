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
#      relocations are R_VAX_RELATIVE plus only the documented libc/libgcc
#      helper imports (__udiv/__urem/__fstat50 — resolved when IMGACT.EXE is
#      finalized as a runnable PT_INTERP in P4);
#   3. the freestanding VAX syscall primitive (syscall6, arch/vax/start.S) is
#      structurally correct in isolation — it issues `chmk`, tests the carry
#      flag, and returns -errno (the NetBSD/vax syscall convention); and
#   4. the toolchain emits a REAL elf32-vax object carrying a `.vms$sv` symbol
#      vector (the container LINK.EXE will produce in P3), readelf-verified.
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

echo "== assert: own dynamic relocs are R_VAX_RELATIVE + only known libc/libgcc helpers =="
"$RE" -rW "$IMG" 2>/dev/null | grep -oE "R_VAX_[A-Z_]+" | sort | uniq -c
"$RE" -rW "$IMG" 2>/dev/null | grep -q "R_VAX_RELATIVE" || { echo "FAIL: no R_VAX_RELATIVE self-relocs"; exit 1; }
# Any non-RELATIVE reloc must name one of the documented external helpers.
unexpected=$("$RE" -rW "$IMG" 2>/dev/null \
    | grep -E "R_VAX_(JMP_SLOT|GLOB_DAT|32)" \
    | grep -vE "__udiv|__urem|__fstat50" || true)
if [ -n "$unexpected" ]; then
    echo "FAIL: unexpected external relocation(s):"; echo "$unexpected"; exit 1
fi
echo "ok: relocs are RELATIVE + documented helpers only"

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
echo "PASS: elf32-vax IMGACT backend builds + is a clean static-PIE; syscall6 ABI"
echo "      shape verified; toolchain emits elf32-vax .vms\$sv. (Runtime activation"
echo "      on the SIMH rail is P4.)"
