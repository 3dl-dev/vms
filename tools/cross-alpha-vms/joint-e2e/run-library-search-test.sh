#!/bin/bash
# run-library-search-test.sh -- LINK.EXE EVAX object-library search + long
# names (vms-4d0). Link-only, toolchain container, no boot.
#
#   tools/cross-alpha-vms/joint-e2e/run-library-search-test.sh <joint-out-dir>
#
# <joint-out-dir> is a build-joint-image.sh OUT dir (it carries LINK.EXE,
# crt0.obj, DECC$SHR.EXE, LIBOTS_SHR.EXE). The test compiles libsearch/{main,a,b,c}.c
# with the real alpha-dec-vms cross cc1, archives a.obj + b.obj + c.obj into a
# System V `ar` library, and asserts:
#   1. --library LIB links pulling exactly the two needed members ("2 of 3"):
#      a.obj and c.obj define routines whose names differ only past character
#      31, so a linker that clamps names at 31 sees one symbol and pulls one
#      member (silently binding both calls to it); b.obj is never pulled;
#   2. the same archive as a plain input (whole-archive) FAILS %LINK-F-UNDEF on
#      b.obj's unresolvable reference -- the search is genuinely selective;
#   3. a library without a.obj FAILS %LINK-F-UNDEF on main's reference -- the
#      search does not invent definitions.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(cd "${1:?usage: $0 <joint-out-dir>}" && pwd)
IMG=${IMG:-ovmx-cross-alpha-vms}
# LINK_EXE (optional, a path inside the OUT dir as /out/...) runs the test against
# a different LINK.EXE -- used to show the test fails on a 31-char-clamping linker.
export LINK_EXE=${LINK_EXE:-}
for f in LINK.EXE crt0.obj 'DECC$SHR.EXE' LIBOTS_SHR.EXE; do
  [ -s "$OUT/$f" ] || { echo "FAIL: $OUT/$f missing (run build-joint-image.sh first)"; exit 1; }
done
docker run --rm -v "$HERE/libsearch:/ls:ro" -v "$OUT:/out" -e LINK_EXE "$IMG" bash -c '
set -uo pipefail
export PATH=/opt/cross-alpha-vms/bin:$PATH
W=/out/libsearch; rm -rf "$W"; mkdir -p "$W"; cd "$W"
for s in main a b c; do
  alpha-dec-vms-gcc -mpointer-size=64 -g0 -c /ls/$s.c -o $s.obj || { echo "FAIL: compile $s.c"; exit 1; }
done
ar rcS libsearch.a a.obj b.obj c.obj
ar rcS libnoa.a b.obj c.obj
L="${LINK_EXE:-/out/LINK.EXE} --transfer __main --use /out/DECC\$SHR.EXE --use /out/LIBOTS_SHR.EXE"
fails=0

echo "-- 1: --library pulls exactly the 2 needed members; >31-char names stay distinct --"
if $L -o ok.exe /out/crt0.obj main.obj --library libsearch.a > t1.log 2>&1 \
   && grep -q "LINK-I-LIBRARY, libsearch.a: 2 of 3 members pulled" t1.log \
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
     && grep -q "LINK-I-LIBRARY, libnoa.a: 1 of 2 members pulled" t3.log; then
  echo "  PASS (rejected: %LINK-F-UNDEF on alpha; only c.obj pulled)"
else
  echo "  FAIL: wrong failure"; cat t3.log; fails=$((fails+1))
fi

[ "$fails" -eq 0 ] && echo "LIBRARY SEARCH TEST PASS (vms-4d0)" || { echo "LIBRARY SEARCH TEST FAIL ($fails)"; exit 1; }
'
