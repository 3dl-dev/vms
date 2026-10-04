#!/bin/bash
# run-library-search-test.sh -- LINK.EXE EVAX object-library search + long
# names (vms-4d0). Link-only, toolchain container, no boot.
#
#   tools/cross-alpha-vms/joint-e2e/run-library-search-test.sh <joint-out-dir>
#
# <joint-out-dir> is a build-joint-image.sh OUT dir (it carries LINK.EXE,
# crt0.obj, DECC$SHR.EXE, LIBOTS_SHR.EXE). The test compiles libsearch/{main,a,b}.c
# with the real alpha-dec-vms cross cc1, archives a.obj + b.obj into a System V
# `ar` library, and asserts:
#   1. --library LIB links: only the needed member is pulled ("1 of 2"), and the
#      two routines whose names differ only past character 31 resolve as two
#      distinct definitions (a 31-char name clamp makes them a duplicate);
#   2. the same archive as a plain input (whole-archive) FAILS %LINK-F-UNDEF on
#      b.obj's unresolvable reference -- the search is genuinely selective;
#   3. a library without a.obj FAILS %LINK-F-UNDEF on main's reference -- the
#      search does not invent definitions.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(cd "${1:?usage: $0 <joint-out-dir>}" && pwd)
IMG=${IMG:-ovmx-cross-alpha-vms}
for f in LINK.EXE crt0.obj 'DECC$SHR.EXE' LIBOTS_SHR.EXE; do
  [ -s "$OUT/$f" ] || { echo "FAIL: $OUT/$f missing (run build-joint-image.sh first)"; exit 1; }
done
docker run --rm -v "$HERE/libsearch:/ls:ro" -v "$OUT:/out" "$IMG" bash -c '
set -uo pipefail
export PATH=/opt/cross-alpha-vms/bin:$PATH
W=/out/libsearch; rm -rf "$W"; mkdir -p "$W"; cd "$W"
for s in main a b; do
  alpha-dec-vms-gcc -mpointer-size=64 -g0 -c /ls/$s.c -o $s.obj || { echo "FAIL: compile $s.c"; exit 1; }
done
ar rcS libsearch.a a.obj b.obj
ar rcS libnoa.a b.obj
L="/out/LINK.EXE --transfer __main --use /out/DECC\$SHR.EXE --use /out/LIBOTS_SHR.EXE"
fails=0

echo "-- 1: --library pulls only the needed member; >31-char names stay distinct --"
if $L -o ok.exe /out/crt0.obj main.obj --library libsearch.a > t1.log 2>&1 \
   && grep -q "LINK-I-LIBRARY, libsearch.a: 1 of 2 members pulled" t1.log \
   && grep -q "LINK-S-CREATED" t1.log && ! grep -q "LINK-F" t1.log; then
  echo "  PASS"
else
  echo "  FAIL"; cat t1.log; fails=$((fails+1))
fi

echo "-- 2: the same archive whole-archived must FAIL (b.obj has an unresolvable ref) --"
if $L -o whole.exe /out/crt0.obj main.obj libsearch.a > t2.log 2>&1; then
  echo "  FAIL: whole-archive link succeeded"; fails=$((fails+1))
elif grep -q "LINK-F-UNDEF.*ovmx_library_search_never_defined" t2.log; then
  echo "  PASS (rejected: %LINK-F-UNDEF ovmx_library_search_never_defined)"
else
  echo "  FAIL: wrong failure"; cat t2.log; fails=$((fails+1))
fi

echo "-- 3: a library lacking the defining member must FAIL --"
if $L -o noa.exe /out/crt0.obj main.obj --library libnoa.a > t3.log 2>&1; then
  echo "  FAIL: link succeeded without a definition"; fails=$((fails+1))
elif grep -q "LINK-F-UNDEF.*ovmx_library_search_long_routine_name_" t3.log \
     && grep -q "LINK-I-LIBRARY, libnoa.a: 0 of 1 member pulled" t3.log; then
  echo "  PASS (rejected: %LINK-F-UNDEF, 0 of 1 pulled)"
else
  echo "  FAIL: wrong failure"; cat t3.log; fails=$((fails+1))
fi

[ "$fails" -eq 0 ] && echo "LIBRARY SEARCH TEST PASS (vms-4d0)" || { echo "LIBRARY SEARCH TEST FAIL ($fails)"; exit 1; }
'
