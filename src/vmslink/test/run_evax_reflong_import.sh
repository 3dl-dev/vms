#!/bin/sh
# run_evax_reflong_import.sh -- LINK.EXE binds a cross-image REFLONG import
# (vms-reflong): a DEC C default (32-bit pointer) consumer that statically
# initialises a procedure value of an IMPORTED routine
# (evax-fixtures/reflong_main.c: int (*helper_ptr)(void) = HELPER_PROC;) gets a
# longword .vms$imp cell (OVMX_IMP_LONG = both form bits) that IMGACT fills
# at activation. LINK used to die "cross-image REFLONG import unsupported"
# (the GCC host build's first such failure). Host-only. Exit 0 = success.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/.." && pwd)
FIX="$HERE/evax-fixtures"
WORK=${WORK:-/tmp/evax-reflong-test}
rm -rf "$WORK"; mkdir -p "$WORK"
$CC -std=gnu11 -O2 -I"$SRC/include" -o "$WORK/LINK.EXE" "$SRC/link.c" 2>/dev/null
printf 'int HELPER_PROC(void) { return 42; }\n' > "$WORK/helper.c"
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/helper.o" "$WORK/helper.c"
"$WORK/LINK.EXE" --shareable --symbol-vector "HELPER_PROC=PROCEDURE" \
    --gsmatch LEQUAL,1,0 -o "$WORK/HELPER\$SHR.EXE" "$WORK/helper.o" 2>/dev/null
"$WORK/LINK.EXE" --transfer MAIN_PROC --use "$WORK/HELPER\$SHR.EXE" \
    -o "$WORK/main.exe" "$FIX/reflong_main.obj" 2>"$WORK/link.log" \
    || { cat "$WORK/link.log"; echo "FAIL: LINK refused the cross-image REFLONG"; exit 1; }
python3 - "$WORK/main.exe" "$SRC/include/ovmx_image.h" <<'PYEOF'
import re, struct, sys
d = open(sys.argv[1], 'rb').read()
hdr = open(sys.argv[2]).read()
LINK = int(re.search(r'#define OVMX_IMP_LINKAGE\s+(0x[0-9a-fA-F]+)u', hdr).group(1), 16)
CODE = int(re.search(r'#define OVMX_IMP_CODEADDR\s+(0x[0-9a-fA-F]+)u', hdr).group(1), 16)
e_shoff, = struct.unpack_from('<Q', d, 0x28)
shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x3a)
sh = [struct.unpack_from('<IIQQQQIIQQ', d, e_shoff + i * shentsize) for i in range(shnum)]
st = sh[shstrndx]
def nm(o):
    s = d[st[4] + o:]; return s[:s.index(b'\0')].decode()
secs = {nm(s[0]): s for s in sh[1:]}
def ok(c, m):
    print(('  PASS ' if c else '  FAIL ') + m)
    if not c: sys.exit(1)
ok('.vms$imp' in secs, 'the image carries a .vms$imp table')
imp = secs['.vms$imp']
magic, count = struct.unpack_from('<II', d, imp[4])
hsz = 16
ents = [struct.unpack_from('<IIQII', d, imp[4] + hsz + 24 * i) for i in range(count)]
longs = [e for e in ents if (e[1] & (LINK | CODE)) == (LINK | CODE)]
ok(len(longs) == 1, 'one import carries the LONG form (both form bits): %d' % len(longs))
off = longs[0][2]
def foff(va):
    for s in secs.values():
        if s[1] != 8 and s[3] <= va < s[3] + s[5]: return s[4] + va - s[3]
    sys.exit('slot outside every section')
ok(struct.unpack_from('<I', d, foff(off))[0] == 0, 'its longword slot is zero until activation (0x%x)' % off)
PYEOF
echo "ALL EVAX REFLONG CROSS-IMAGE IMPORT CHECKS PASSED"
