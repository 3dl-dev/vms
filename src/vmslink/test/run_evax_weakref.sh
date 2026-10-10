#!/bin/sh
# run_evax_weakref.sh -- LINK.EXE's EVAX/Alpha path turns a WEAK reference
# (EGSY$V_WEAK on the object's ESRF, what `#pragma weak` emits) that nothing in
# the link defines into a weak-by-name import (.vms$wimp), as the ELF path
# does (vms-5f0), instead of leaving the cell 0 at link (vms-e1a7). That is how
# LIBVMS$SHR reaches LIBVMSRMS$SHR (sys$parse/sys$search, ovmx_rightslist_*)
# on Alpha; with the cell baked to 0, LIB$FIND_FILE returned SS$_NOSUCHDEV and
# FAO !%I printed [1,4] for every Alpha image.
#
# Checks, on the checked-in fixture evax-fixtures/weakref_main.obj:
#   A. linked with a --use producer that exports HELPER_PROC: HELPER_PROC is an
#      ordinary .vms$imp import; WEAK_CALL / WEAK_DATA are .vms$wimp records,
#      the linkage-pair one carrying OVMX_IMP_LINKAGE, the data ones form 0,
#      every cell 0 until activation; no weak name appears in .vms$imp.
#   B. linked with no producer and without --allow-undefined: links (a weak
#      reference is not an undefined symbol), every reference in .vms$wimp, no
#      .vms$imp table.
#   C. a STRONG undefined reference still fails %LINK-F-UNDEF (link_main.obj).
# Host-only, byte checks. Exit 0 = pass.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/.." && pwd)
FIX="$HERE/evax-fixtures"
WORK=${WORK:-/tmp/evax-weakref-test}
rm -rf "$WORK"; mkdir -p "$WORK"
$CC -std=gnu11 -O2 -I"$SRC/include" -o "$WORK/LINK.EXE" "$SRC/link.c" 2>/dev/null
printf 'int HELPER_PROC(void) { return 42; }\n' > "$WORK/helper.c"
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/helper.o" "$WORK/helper.c"
"$WORK/LINK.EXE" --shareable --symbol-vector "HELPER_PROC=PROCEDURE" \
    --gsmatch LEQUAL,1,0 -o "$WORK/HELPER\$SHR.EXE" "$WORK/helper.o" 2>/dev/null

"$WORK/LINK.EXE" --transfer MAIN_PROC --use "$WORK/HELPER\$SHR.EXE" \
    -o "$WORK/a.exe" "$FIX/weakref_main.obj" 2>"$WORK/a.log" \
    || { cat "$WORK/a.log"; echo "FAIL: A. LINK refused the weak references"; exit 1; }
"$WORK/LINK.EXE" --transfer MAIN_PROC \
    -o "$WORK/b.exe" "$FIX/weakref_main.obj" 2>"$WORK/b.log" \
    || { cat "$WORK/b.log"; echo "FAIL: B. a weak reference was treated as undefined"; exit 1; }
if grep -q 'LINK-W-UNDEF' "$WORK/a.log" "$WORK/b.log"; then
    echo "FAIL: a weak reference was still left 0 at link (LINK-W-UNDEF)"; exit 1
fi

python3 - "$WORK/a.exe" "$WORK/b.exe" "$SRC/include/ovmx_image.h" <<'PYEOF'
import re, struct, sys
hdr = open(sys.argv[3]).read()
def const(n):
    return int(re.search(r'#define %s\s+(0x[0-9a-fA-F]+)u' % n, hdr).group(1), 16)
LINK, CODE, WMAGIC = const('OVMX_IMP_LINKAGE'), const('OVMX_IMP_CODEADDR'), const('OVMX_WIMP_MAGIC')
def ok(c, m):
    print(('  PASS ' if c else '  FAIL ') + m)
    if not c: sys.exit(1)
def load(path):
    d = open(path, 'rb').read()
    e_shoff, = struct.unpack_from('<Q', d, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x3a)
    sh = [struct.unpack_from('<IIQQQQIIQQ', d, e_shoff + i * shentsize) for i in range(shnum)]
    st = sh[shstrndx]
    def nm(o):
        s = d[st[4] + o:]; return s[:s.index(b'\0')].decode()
    return d, {nm(s[0]): s for s in sh[1:]}
def cell(d, secs, va, n):
    for name, s in secs.items():
        if name.startswith('.vms$') or s[1] == 8: continue
        if s[3] <= va < s[3] + s[5]:
            return d[s[4] + va - s[3]: s[4] + va - s[3] + n]
    sys.exit('cell 0x%x outside every section' % va)
def wimp(d, secs):
    w = secs['.vms$wimp']; base = w[4]
    magic, count, noff, nsz = struct.unpack_from('<IIII', d, base)
    ok(magic == WMAGIC, '.vms$wimp magic')
    out = []
    for i in range(count):
        no, form, patch = struct.unpack_from('<IIQ', d, base + 16 + 16 * i)
        s = d[base + noff + no:]; out.append((s[:s.index(b'\0')].decode(), form, patch))
    return out
def imp_count(d, secs):
    if '.vms$imp' not in secs: return 0
    return struct.unpack_from('<II', d, secs['.vms$imp'][4])[1]

print('A. weak references beside a --use producer')
d, secs = load(sys.argv[1])
ok('.vms$wimp' in secs, 'the image carries a .vms$wimp table')
w = wimp(d, secs)
names = sorted(n for n, _, _ in w)
ok('HELPER_PROC' not in names, 'HELPER_PROC (exported by the producer) is not a weak import')
ok(set(names) == {'WEAK_CALL', 'WEAK_DATA'}, 'weak records name WEAK_CALL / WEAK_DATA: %s' % names)
ok(imp_count(d, secs) == 1, 'exactly one strong .vms$imp record (HELPER_PROC)')
lk = [r for r in w if r[1] & LINK and not r[1] & CODE]
ok(len(lk) == 1 and lk[0][0] == 'WEAK_CALL', 'the call is a LINKAGE-form weak record')
ok(cell(d, secs, lk[0][2], 16) == bytes(16), 'its linkage pair is 0 until activation')
data = [r for r in w if r[1] == 0]
ok(len(data) == len(w) - 1, 'the presence tests are raw-value (form 0) records')
for n, f, p in data:
    ok(cell(d, secs, p, 8) == bytes(8), '%s data cell 0x%x is 0 until activation' % (n, p))

print('B. no producer, no --allow-undefined')
d, secs = load(sys.argv[2])
w = wimp(d, secs)
ok({n for n, _, _ in w} == {'WEAK_CALL', 'WEAK_DATA', 'HELPER_PROC'},
   'every weak reference is a .vms$wimp record')
ok('.vms$imp' not in secs, 'no .vms$imp table (no strong import)')
PYEOF

echo "C. a strong undefined reference still fails"
if "$WORK/LINK.EXE" --transfer MAIN_PROC -o "$WORK/c.exe" "$FIX/link_main.obj" \
        > "$WORK/c.log" 2>&1; then
    echo "  FAIL link_main.obj (strong HELPER_PROC, no producer) linked"; exit 1
fi
grep -q '%LINK-F-UNDEF' "$WORK/c.log" || { cat "$WORK/c.log"; echo "  FAIL expected %LINK-F-UNDEF"; exit 1; }
echo "  PASS strong undefined -> %LINK-F-UNDEF"
echo "ALL EVAX WEAK-REFERENCE CHECKS PASSED"
