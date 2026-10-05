#!/bin/sh
# run_evax_zeropsect.sh -- LINK.EXE places a ZERO-length psect contribution
# (vms-4d0).
#
# A zero-length contribution is a position marker: crtbegin's empty eh_frame
# contribution is __EH_FRAME_BEGIN__, the start of the merged eh_frame psect
# that crtbegin hands to __register_frame_info. VMS LINK places it where it
# falls among the psect's contributions. LINK used to skip it, so a reference
# to it resolved to 0 -- no EH frames were registered and every C++ throw
# terminated. Fixtures (evax-fixtures/zmark_*.c, alpha-dec-vms-gcc
# -mpointer-size=64): zmark_beg contributes 0 bytes to psect "zmark" and holds
# zmark_ptr -> that contribution; zmark_dat contributes the 8-byte zmark_val.
#   - beg + dat: zmark_ptr == the zmark section address, which holds zmark_val;
#   - beg alone (the psect has only a zero-length contribution): zmark_ptr is a
#     real image address, never 0.
# Host-only (checked-in EVAX fixtures), no Alpha toolchain. Exit 0 = success.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/.." && pwd)
FIX="$HERE/evax-fixtures"
WORK=${WORK:-/tmp/evax-zeropsect-test}
rm -rf "$WORK"; mkdir -p "$WORK"

$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/LINK.EXE" "$SRC/link.c"
"$WORK/LINK.EXE" --transfer main -o "$WORK/both.exe" "$FIX/zmark_beg.obj" "$FIX/zmark_dat.obj" 2>"$WORK/both.log"
"$WORK/LINK.EXE" --transfer main -o "$WORK/alone.exe" "$FIX/zmark_beg.obj" 2>"$WORK/alone.log"

python3 - "$WORK/both.exe" "$WORK/alone.exe" <<'PYEOF'
import struct, sys
def load(p):
    d = open(p, 'rb').read()
    e_shoff, = struct.unpack_from('<Q', d, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', d, 0x3a)
    sh = [struct.unpack_from('<IIQQQQIIQQ', d, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    st = sh[e_shstrndx]
    def nm(o):
        s = d[st[4] + o:]; return s[:s.index(b'\0')].decode()
    return d, {nm(s[0]): s for s in sh[1:]}
def ok(c, m):
    print(('  PASS ' if c else '  FAIL ') + m)
    if not c: sys.exit(1)
d, sec = load(sys.argv[1])
ok('zmark' in sec and '$DATA$' in sec, 'both: zmark and $DATA$ sections present')
z, dat = sec['zmark'], sec['$DATA$']
ptr = struct.unpack_from('<Q', d, dat[4])[0]
val = struct.unpack_from('<Q', d, z[4])[0]
ok(ptr == z[3], 'both: zmark_ptr 0x%x == zmark section address 0x%x (marker placed, not 0)' % (ptr, z[3]))
ok(val == 0x5a4d41524b5a4d, 'both: the marker addresses zmark_val (0x%x)' % val)
d, sec = load(sys.argv[2])
dat = sec['$DATA$']
ptr = struct.unpack_from('<Q', d, dat[4])[0]
lo = min(s[3] for n, s in sec.items() if s[3])
hi = max(s[3] + s[5] for n, s in sec.items() if s[3])
ok(ptr != 0 and lo <= ptr <= hi, 'alone: zmark_ptr 0x%x is a real image address (image 0x%x..0x%x)' % (ptr, lo, hi))
PYEOF
echo "ALL EVAX ZERO-LENGTH PSECT (vms-4d0) CHECKS PASSED"
