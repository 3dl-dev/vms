#!/bin/bash
# test_ods2_header_tree.sh -- ods2-header-tree.py puts a header tree on ODS-2
# legal names and writes the header.gcc pairs GCC's -remap reads; it refuses a
# tree it cannot place (an illegal directory name, two names on one ODS-2 name).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fails=0
ok() { echo "  PASS: $1"; }
bad() { echo "  FAIL: $1"; fails=$((fails + 1)); }

mkdir -p "$T/src/bits" "$T/src/ext/pb_ds"
echo cfg > "$T/src/bits/c++config.h"
echo str > "$T/src/bits/basic_string.h"
echo ios > "$T/src/iostream"
echo pb  > "$T/src/ext/pb_ds/cc_hash_max_collision_check_resize_trigger_imp.hpp"
echo all > "$T/src/stdc++.h"
echo dot > "$T/src/a.b.h"
python3 -I "$HERE/ods2-header-tree.py" "$T/src" "$T/out" > "$T/log" 2>&1 && ok "a legal tree is placed" || { bad "rc != 0"; cat "$T/log"; }
[ "$(cat "$T/out/bits/cxxconfig.h" 2>/dev/null)" = cfg ] && ok "c++config.h -> CXXCONFIG.H" || bad "no cxxconfig.h"
grep -qx 'c++config.h cxxconfig.h' "$T/out/bits/header.gcc" 2>/dev/null && ok "bits/header.gcc maps it" || bad "bits/header.gcc"
grep -q basic_string "$T/out/bits/header.gcc" && bad "a legal name was remapped" || ok "a legal name is not remapped"
# every directory carries every pair (a search dir without the file says "not here")
grep -qx 'c++config.h cxxconfig.h' "$T/out/ext/pb_ds/header.gcc" && grep -qx 'stdc++.h stdcxx.h' "$T/out/bits/header.gcc" \
  && ok "every directory's header.gcc carries every pair" || bad "pairs are not in every directory"
[ -f "$T/out/iostream" ] && [ -f "$T/out/bits/basic_string.h" ] && ok "legal names kept" || bad "legal names"
grep -qx 'stdc++.h stdcxx.h' "$T/out/header.gcc" && [ -f "$T/out/stdcxx.h" ] && ok "stdc++.h -> STDCXX.H" || bad "stdc++.h"
# a.b.h: the C RTL itself looks up A_B.H, so the file goes there and needs no pair
[ -f "$T/out/a_b.h" ] && ! grep -q '^a\.b\.h' "$T/out/header.gcc" && ok "a.b.h -> A_B.H, no pair" || bad "a.b.h"
n=$(sed -n 's/^cc_hash_max_collision_check_resize_trigger_imp.hpp //p' "$T/out/ext/pb_ds/header.gcc")
s=${n%.hpp}
[ -n "$n" ] && [ ${#s} -le 39 ] && [ -f "$T/out/ext/pb_ds/$n" ] && ok "a 46-character name -> $n" || bad "long name ($n)"
python3 -I - "$T/out" <<'EOF' && ok "every placed name is ODS-2 legal" || bad "an illegal name was placed"
import os, re, sys
L = re.compile(r'^[a-z0-9$_-]{1,39}(\.[a-z0-9$_-]{0,39})?$')
bad = [f for r, d, fs in os.walk(sys.argv[1]) for f in fs + d if not L.match(f)]
print(bad) if bad else None
sys.exit(1 if bad else 0)
EOF

python3 -I "$HERE/ods2-header-tree.py" "$T/src" "$T/out2" --relative-pairs "$T/rel.txt" > /dev/null 2>&1 \
  && grep -qx 'bits/c++config.h bits/cxxconfig.h' "$T/rel.txt" && grep -qx 'c++config.h cxxconfig.h' "$T/rel.txt" \
  && grep -qx 'stdc++.h stdcxx.h' "$T/rel.txt" && [ "$(grep -c '^c++config.h ' "$T/rel.txt")" = 1 ] \
  && ok "--relative-pairs lists dir/name pairs for other include directories" || bad "--relative-pairs"
mkdir -p "$T/s2/c++"; echo x > "$T/s2/c++/x.h"
python3 -I "$HERE/ods2-header-tree.py" "$T/s2" "$T/o2" > /dev/null 2>&1 && bad "an illegal directory name accepted" || ok "an illegal directory name is refused"
mkdir -p "$T/s3"; echo 1 > "$T/s3/a+b.h"; echo 2 > "$T/s3/axb.h"
python3 -I "$HERE/ods2-header-tree.py" "$T/s3" "$T/o3" > /dev/null 2>&1 && bad "a collision accepted" || ok "two names on one ODS-2 name are refused"

[ "$fails" -eq 0 ] && { echo "test_ods2_header_tree: PASS"; exit 0; }
echo "test_ods2_header_tree: $fails FAIL"; exit 1
