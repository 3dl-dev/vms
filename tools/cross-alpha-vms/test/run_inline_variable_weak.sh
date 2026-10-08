#!/bin/bash
# run_inline_variable_weak.sh -- the stage-2 alpha-dec-vms g++ emits an
# initialized C++17 inline variable as a WEAK definition (vms-9a63,
# patches/0013), the same linkage it gives a template static data member, so
# two objects that both define it link. alpha-dec-vms has weak symbols but no
# COMDAT groups; unpatched, GCC chose COMMON before the initializer was seen
# and emitted a strong .globl in every object (%LINK-F-MULDEF tree_code_type
# linking cc1). The control -- the template static member -- is weak either way.
# Usage: bash run_inline_variable_weak.sh <g++>
set -euo pipefail
GXX=$1
W=$(mktemp -d)
cat > "$W/iv.cc" <<'C'
constexpr inline int ovmx_tct[] = { 1, 2, 3 };
inline int ovmx_tv = 5;
template <int N> struct S { static int v; };
template <int N> int S<N>::v = N;
int f (int i) { return ovmx_tct[i] + ovmx_tv + S<3>::v; }
C
"$GXX" -std=c++17 -O2 -S -o "$W/iv.s" "$W/iv.cc"
rc=0
for sym in ovmx_tct ovmx_tv _ZN1SILi3EE1vE; do
    if grep -qE "^[[:space:]]*\.weak[[:space:]]+$sym\$" "$W/iv.s" && ! grep -qE "^[[:space:]]*\.globl[[:space:]]+$sym\$" "$W/iv.s"; then
        echo "  PASS $sym is a weak definition"
    else
        echo "  FAIL $sym is not emitted weak:"; grep -nE "[[:space:]]$sym\$" "$W/iv.s" | sed 's/^/      /'; rc=1
    fi
done
rm -rf "$W"
[ "$rc" -eq 0 ] && echo "ALL INLINE-VARIABLE LINKAGE (vms-9a63) CHECKS PASSED"
exit $rc
