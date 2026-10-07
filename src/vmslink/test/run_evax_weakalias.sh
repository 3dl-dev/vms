#!/bin/sh
# run_evax_weakalias.sh -- LINK.EXE resolves a same-TU weak override by the
# address a reloc names, not by its section base (vms-122).
#
# musl's mmap.c/munmap.c/mremap.c begin with a weak no-op (`__vm_wait`, aliased
# to a static dummy at offset 0 of $CODE$) that the pthread code overrides
# strongly, followed by the real routine, which a weak alias exports under its
# DEC C name (decc$_mmap64 ...). The alpha back end emits section-relative
# relocs inside the object. LINK used to apply the weak->strong redirect to the
# SECTION BASE -- which equals the weak dummy's code entry -- so every sibling's
# descriptor entry-field moved into the strong __vm_wait's code, and the
# exported mmap/munmap entered __vm_wait (a no-op returning stale R0).
# Fixtures: evax-fixtures/weakalias_lib.c (that shape: dummy/__vm_wait, __impl,
# weak alias mmap -> decc$_mmap64) + weakalias_strong.c (strong __vm_wait).
#   - the decc$_mmap64 universal exports __impl's own descriptor;
#   - __impl's descriptor entry-field is __impl's code, not strong __vm_wait's;
#   - __impl's call to __vm_wait still reaches the STRONG __vm_wait.
# Host-only (checked-in EVAX fixtures). Exit 0 = success.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/.." && pwd)
FIX="$HERE/evax-fixtures"
WORK=${WORK:-/tmp/evax-weakalias-test}
rm -rf "$WORK"; mkdir -p "$WORK"

$CC -std=gnu11 -O2 -I"$SRC/include" -o "$WORK/LINK.EXE" "$SRC/link.c" 2>/dev/null
"$WORK/LINK.EXE" --shareable \
    --symbol-vector 'decc$_mmap64=PROCEDURE,__impl=PROCEDURE,__vm_wait=PROCEDURE' \
    --gsmatch LEQUAL,1,0 -o "$WORK/WA\$SHR.EXE" \
    "$FIX/weakalias_lib.obj" "$FIX/weakalias_strong.obj" 2>"$WORK/link.log"

python3 - "$WORK/WA\$SHR.EXE" <<'PYEOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
e_shoff, = struct.unpack_from('<Q', d, 0x28)
shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x3a)
sh = [struct.unpack_from('<IIQQQQIIQQ', d, e_shoff + i * shentsize) for i in range(shnum)]
st = sh[shstrndx]
def nm(o):
    s = d[st[4] + o:]; return s[:s.index(b'\0')].decode()
secs = {nm(s[0]): s for s in sh[1:]}
sv = secs['.vms$sv']
magic, count, gk, gmaj, gmin, names_off, names_size, _ = struct.unpack_from('<8I', d, sv[4])
vals = {}
for i in range(count):
    value, kind, name_off = struct.unpack_from('<QII', d, sv[4] + 32 + 16 * i)
    nb = d[sv[4] + names_off + name_off:]
    vals[nb[:nb.index(b'\0')].decode()] = value
def ok(c, m):
    print(('  PASS ' if c else '  FAIL ') + m)
    if not c: sys.exit(1)
def off(va):            # image-relative address -> file offset (identity-mapped sections)
    for s in secs.values():
        if s[3] <= va < s[3] + s[5] and s[1] != 8: return s[4] + (va - s[3])
    raise SystemExit('address 0x%x outside every section' % va)
entry = lambda pv: struct.unpack_from('<Q', d, off(pv) + 8)[0]
ok(vals['decc$_mmap64'] == vals['__impl'],
   'decc$_mmap64 exports __impl\'s own descriptor (0x%x == 0x%x)' % (vals['decc$_mmap64'], vals['__impl']))
ok(entry(vals['__impl']) != entry(vals['__vm_wait']),
   '__impl\'s entry-field 0x%x is its own code, not strong __vm_wait\'s 0x%x' % (entry(vals['__impl']), entry(vals['__vm_wait'])))
code = secs['$CODE$']
ok(code[3] <= entry(vals['__impl']) < code[3] + code[5], '__impl\'s entry lies in $CODE$')
# __impl's linkage pair for its __vm_wait call must name the STRONG def.
link = secs['$LINK$']
strong_pv, strong_entry = vals['__vm_wait'], entry(vals['__vm_wait'])
found = False
for o in range(0, link[5] - 15, 8):
    q0, q1 = struct.unpack_from('<QQ', d, link[4] + o)
    if q1 == strong_pv and q0 == strong_entry: found = True
ok(found, 'the same-TU call to the weak __vm_wait is bound to the strong __vm_wait (pair {0x%x,0x%x})' % (strong_entry, strong_pv))
PYEOF
echo "ALL EVAX WEAK-ALIAS / SECTION-RELATIVE OVERRIDE (vms-122) CHECKS PASSED"
