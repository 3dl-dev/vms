#!/bin/bash
# run_p32_eh_encoding.sh -- the alpha-dec-vms port compiler encodes the EH
# addresses of 32-bit-pointer code as 8-byte values (vms-1045, patches/0007).
# Runs INSIDE the ovmx-cross-alpha-vms image:
#   docker run --rm -v <repo>/tools/cross-alpha-vms/test:/t:ro ovmx-cross-alpha-vms bash /t/run_p32_eh_encoding.sh
# libgcc's unwinder is always 64-bit on VMS (t-vms) and reads DW_EH_PE_absptr
# as 8 bytes. A C TU with -fexceptions emits a CIE + FDE; at the default
# (32-bit) pointer size the CIE must carry an explicit FDE encoding of
# DW_EH_PE_udata8 ("zR", 0x4) and the FDE pc_begin must be a .quad.
# Unpatched: no R augmentation and a 4-byte .long pc_begin. The 64-bit form is
# unchanged (absptr, .quad).
set -euo pipefail
CC=${CC:-/opt/cross-alpha-vms/bin/alpha-dec-vms-gcc}
W=$(mktemp -d)
printf 'extern void g(void);\nvoid f(void (*h)(void)) { h(); g(); }\n' > "$W/t.c"
fde_begin() {   # the directive that emits the first FDE's pc_begin ($LFB0)
    grep -oE '^\s*\.(quad|long)\s+\$LFB0$' "$1" | head -1 | awk '{print $1}'
}
cie_aug() {     # the CIE augmentation string
    sed -n '/\.eh_frame/,/^\$LECIE1:/p' "$1" | grep -oE '\.ascii "[^"]*"' | head -1 | sed 's/.ascii //'
}
fails=0
"$CC" -fexceptions -O1 -S "$W/t.c" -o "$W/p32.s"
"$CC" -mpointer-size=64 -fexceptions -O1 -S "$W/t.c" -o "$W/p64.s"
b32=$(fde_begin "$W/p32.s"); r32=$(cie_aug "$W/p32.s")
b64=$(fde_begin "$W/p64.s"); r64=$(cie_aug "$W/p64.s")
# 32-bit: augmentation "zR" (an explicit FDE encoding, udata8) and an 8-byte pc_begin.
if [ "$b32" = ".quad" ] && [ "$r32" = '"zR\0"' ] && grep -qE '^\s*\.byte\s+0x4$' "$W/p32.s"; then
    echo "  PASS 32-bit pointers: FDE pc_begin $b32, CIE augmentation $r32 with FDE encoding udata8 (0x4)"
else
    echo "  FAIL 32-bit pointers: FDE pc_begin '$b32', CIE augmentation '$r32' (want .quad, \"zR\" + 0x4)"; fails=1
fi
# 64-bit: unchanged -- the default absptr (no R augmentation), 8 bytes wide.
if [ "$b64" = ".quad" ] && [ "$r64" = '"\0"' ]; then
    echo "  PASS 64-bit pointers: FDE pc_begin $b64, CIE augmentation $r64 (absptr, unchanged)"
else
    echo "  FAIL 64-bit pointers: FDE pc_begin '$b64', CIE augmentation '$r64' (want .quad, \"\")"; fails=1
fi
rm -rf "$W"
[ "$fails" -eq 0 ] && echo "ALL P32 EH ENCODING (vms-1045) CHECKS PASSED"
