#!/bin/sh
# run_evax_p0base.sh -- LINK.EXE --base: an EVAX/Alpha executable linked at a
# fixed OpenVMS Alpha P0 address (vms-035).
#
# On OpenVMS Alpha a main image lives in P0 space from 0x10000, so every image
# address fits a 32-bit (sign-extended longword) slot. `--base 0x10000` links
# the executable there as an ET_EXEC that is never moved. The proof compares it
# with the relocatable (ET_DYN + .vms$rel) link of the SAME object:
#   - ET_EXEC, one PT_LOAD at vaddr 0x10000 / file offset 0, PT_PHDR consistent
#     (so IMGACT's exec_bias is 0), e_entry == .vms$xfer entry[0], inside P0;
#   - NO .vms$rel (nothing to bias);
#   - the section layout is identical (minus .vms$rel), and every slot the relocatable image lists in
#     .vms$rel holds exactly its relocatable value + 0x10000 -- i.e. the P0 image
#     IS the relocatable image pre-biased to 0x10000, no slot missed;
#   - section headers carry P0 addresses with file offsets = addr - 0x10000.
# Must-fail cases: --base on a shareable, a non-64KB-aligned base, a base >= P1.
# Host-only (checked-in EVAX fixtures), no Alpha toolchain. Exit 0 = success.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/.." && pwd)
FIX="$HERE/evax-fixtures"
WORK=${WORK:-/tmp/evax-p0base-test}
rm -rf "$WORK"; mkdir -p "$WORK"

$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/LINK.EXE" "$SRC/link.c"

"$WORK/LINK.EXE" --transfer main -o "$WORK/rel.exe" "$FIX/gdata.obj" 2>"$WORK/rel.log"
"$WORK/LINK.EXE" --transfer main --base 0x10000 -o "$WORK/p0.exe" "$FIX/gdata.obj" 2>"$WORK/p0.log"
grep -q 'ET_EXEC (P0)' "$WORK/p0.log"

python3 - "$WORK/rel.exe" "$WORK/p0.exe" <<'PYEOF'
import struct, sys
B = 0x10000
def load(p):
    d = open(p, 'rb').read()
    (e_type, e_mach, e_ver, e_entry, e_phoff, e_shoff, e_flags, e_ehsize,
     e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from('<HHIQQQIHHHHHH', d, 16)
    ph = [struct.unpack_from('<IIQQQQQQ', d, e_phoff + i * e_phentsize) for i in range(e_phnum)]
    sh = [struct.unpack_from('<IIQQQQIIQQ', d, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    strtab = sh[e_shstrndx]
    def nm(o):
        s = d[strtab[4] + o:]; return s[:s.index(b'\0')].decode()
    secs = {nm(s[0]): s for s in sh[1:]}
    return d, e_type, e_entry, ph, secs
rd, rt, rentry, rph, rsec = load(sys.argv[1])
pd, pt, pentry, pph, psec = load(sys.argv[2])
def ok(c, m):
    print(('  PASS ' if c else '  FAIL ') + m)
    if not c: sys.exit(1)
ok(rt == 3 and pt == 2, 'relocatable link is ET_DYN, --base link is ET_EXEC')
load_ = [p for p in pph if p[0] == 1]
ok(len(load_) == 1 and load_[0][3] == B and load_[0][2] == 0, 'one PT_LOAD at vaddr 0x10000, file offset 0')
phdr = [p for p in pph if p[0] == 6]
ok(len(phdr) == 1 and phdr[0][3] == B + phdr[0][2], 'PT_PHDR vaddr = 0x10000 + offset (IMGACT exec_bias 0)')
ok('.vms$rel' in rsec and '.vms$rel' not in psec, 'relocatable image has .vms$rel; P0 image has none')
x = psec['.vms$xfer']
xent = struct.unpack_from('<Q', pd, x[4] + 16)[0]
ok(xent == pentry and B <= xent < B + load_[0][5] and xent < 0x40000000,
   'e_entry == .vms$xfer entry[0] (0x%x), inside the P0 image' % xent)
ok(pentry == rentry + B, 'transfer address = relocatable transfer + 0x10000')
ok(set(rsec) - {'.vms$rel'} == set(psec), 'same sections apart from .vms$rel (same layout)')
for n, s in psec.items():
    if n == '.shstrtab': continue
    r = rsec.get(n)
    ok(r is not None and s[3] == r[3] + B and s[4] == r[4], 'section %s at 0x%x (relocatable 0x%x), same file offset' % (n, s[3], r[3]))
rel = rsec['.vms$rel']
magic, cnt = struct.unpack_from('<II', rd, rel[4])
ok(cnt >= 1, '.vms$rel lists %d slot(s)' % cnt)
for i in range(cnt):
    off = struct.unpack_from('<Q', rd, rel[4] + 8 + 8 * i)[0]
    rv = struct.unpack_from('<Q', rd, off)[0]
    pv = struct.unpack_from('<Q', pd, off)[0]
    ok(pv == rv + B, 'slot @0x%x: relocatable 0x%x -> P0 0x%x' % (off, rv, pv))
PYEOF

echo "-- must-fail cases --"
fails=0
"$WORK/LINK.EXE" --shareable --symbol-vector "main=PROCEDURE" --gsmatch LEQUAL,1,0 \
    --base 0x10000 -o "$WORK/bad1.exe" "$FIX/gdata.obj" 2>/dev/null && { echo "  FAIL: --base on a shareable accepted"; fails=1; } || echo "  PASS: --base on a shareable rejected"
"$WORK/LINK.EXE" --transfer main --base 0x12345 -o "$WORK/bad2.exe" "$FIX/gdata.obj" 2>/dev/null && { echo "  FAIL: unaligned base accepted"; fails=1; } || echo "  PASS: non-64KB-aligned base rejected"
"$WORK/LINK.EXE" --transfer main --base 0x40000000 -o "$WORK/bad3.exe" "$FIX/gdata.obj" 2>/dev/null && { echo "  FAIL: base in P1 accepted"; fails=1; } || echo "  PASS: base >= 0x40000000 (P1) rejected"
[ "$fails" -eq 0 ]
echo "ALL EVAX P0 --base (vms-035) CHECKS PASSED"
