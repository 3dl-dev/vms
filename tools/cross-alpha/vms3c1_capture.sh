#!/usr/bin/env bash
# vms3c1_capture.sh - vms-3c1 fault-capture + codegen evidence (DIAGNOSTIC, not a gate).
# Runs on the k3s rail (--dind). Produces, in ONE job:
#   (A) disassembly of the alpha-dec-vms mallocng alloc_group / get_meta / free
#       + a struct-layout probe  -> decides the codegen/LLP64 hypotheses;
#   (B) the veneer boot with print-fatal-signals -> the runtime user SIGSEGV
#       pc/ra/sp/faultVA for the base->meta==NULL free-path crash.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; REPO="$(cd "$HERE/../.." && pwd)"; cd "$REPO"
sec(){ echo; echo "############################################################"; echo "## $*"; echo "############################################################"; }

sec "BUILD toolchain images"
docker build -t ovmx-cross-alpha-vms tools/cross-alpha-vms 2>&1 | tail -5
docker build -t ovmx-cross-alpha     tools/cross-alpha     2>&1 | tail -5

sec "(A) mallocng codegen + layout on alpha-dec-vms"
docker run --rm -v "$REPO/tools/cross-alpha-vms/musl-arch:/overlay:ro" -e OVERLAY=/overlay \
  ovmx-cross-alpha-vms bash -c '
set -e
export PATH=/opt/cross-alpha-vms/bin:$PATH
bash /overlay/build-musl.sh >/tmp/musl.log 2>&1 || { echo "build-musl FAILED"; tail -30 /tmp/musl.log; exit 1; }
cd /tmp/musl-build/musl-1.2.5
INC="-nostdinc -I./arch/alpha-dec-vms -I./arch/generic -Iobj/src/internal -I./src/include -I./src/internal -Iobj/include -I./include -I./src/malloc/mallocng"
echo "=== sizeof/offsetof probe (alpha-dec-vms LLP64) ==="
cat > /tmp/probe.c <<EOF
#include "meta.h"
char P_sizeof_ptr[sizeof(void*)];
char P_sizeof_ulong[sizeof(unsigned long)];
char P_sizeof_uintptr[sizeof(uintptr_t)];
char P_sizeof_meta[sizeof(struct meta)];
char P_sizeof_group[sizeof(struct group)];
char P_off_meta_prev[__builtin_offsetof(struct meta,prev)];
char P_off_meta_next[__builtin_offsetof(struct meta,next)];
char P_off_meta_mem[__builtin_offsetof(struct meta,mem)];
char P_off_meta_availmask[__builtin_offsetof(struct meta,avail_mask)];
char P_off_group_meta[__builtin_offsetof(struct group,meta)];
char P_off_group_activeidx[__builtin_offsetof(struct group,active_idx)];
char P_off_group_storage[__builtin_offsetof(struct group,storage)];
EOF
alpha-dec-vms-gcc -mpointer-size=64 -O2 -g0 $INC -c /tmp/probe.c -o /tmp/probe.o 2>&1 || echo "PROBE COMPILE FAILED"
echo "--- nm -S (symbol = byte size) ---"
alpha-dec-vms-nm -S /tmp/probe.o 2>/dev/null | grep -i "P_" || objdump -t /tmp/probe.o 2>/dev/null | grep -i "P_"
for fn in alloc_group get_meta free free_group nontrivial_free enframe; do
  for o in obj/src/malloc/mallocng/malloc.o obj/src/malloc/mallocng/free.o; do
    echo "=== objdump -d $o : function-ish $fn (may be static/mangled) ==="
    alpha-dec-vms-objdump -dr "$o" 2>/dev/null | awk -v f="$fn" "BEGIN{p=0} /<.*>:/{p=(index(tolower(\$0),tolower(f))>0)} p{print}" | head -160
  done
done
echo "=== nm malloc.o/free.o symbols ==="
alpha-dec-vms-nm obj/src/malloc/mallocng/malloc.o obj/src/malloc/mallocng/free.o 2>/dev/null | grep -iE "alloc_group|get_meta|free|enframe|meta" | head -40
' 2>&1

sec "(B) veneer boot with print-fatal-signals -> runtime user SIGSEGV registers"
export BOOT_APPEND_EXTRA="ignore_loglevel print-fatal-signals=1 loglevel=8"
export BOOT_TIMEOUT="${BOOT_TIMEOUT:-300}"
bash tools/cross-alpha/run-module-gp-activation-alpha.sh crtl-rms-veneer-gate 2>&1 | tail -40 || true

sec "(B) RAW console (segfault dump: pc/ra/sp/faultVA)"
RAW="$REPO/.boot-cache/alpha-modgp-gate/boot/modgpA.raw"
if [ -f "$RAW" ]; then
  echo "--- grep fault signatures ---"
  grep -naiE "segfault|pc *[=:]|ra *[=:]|sp *[=:]| ps *[=:]|Tainted|Oops|BUG|Code:|gp *[=:]|JOINT_E2E|signal 11|fault|Unable to handle|access|t[0-9] *[=:]|v0|a[0-9] *[=:]" "$RAW" | head -80
  echo "--- 60 lines around first segfault/JOINT_E2E fault ---"
  ln=$(grep -naiE "segfault|terminated abnormally|Unable to handle" "$RAW" | head -1 | cut -d: -f1)
  [ -n "${ln:-}" ] && sed -n "$((ln>30?ln-30:1)),$((ln+30))p" "$RAW"
else
  echo "NO RAW CONSOLE at $RAW"; ls -la "$REPO/.boot-cache/alpha-modgp-gate/boot/" 2>/dev/null || true
fi

sec "(B) translate: objdump/nm the BUILT veneer DECC\$SHR + JOINT_E2E"
BD="$REPO/.boot-cache/alpha-modgp-gate"
find "$BD" -maxdepth 6 \( -iname "DECC*SHR*" -o -iname "JOINT_E2E*" -o -iname "*.map" \) 2>/dev/null | head -20
for img in $(find "$BD" -maxdepth 6 \( -iname "DECC*SHR*" -o -iname "JOINT_E2E.EXE" \) 2>/dev/null | head -4); do
  echo "=== $img ==="
  docker run --rm -v "$(dirname "$img")":/img ovmx-cross-alpha-vms bash -c \
    "alpha-dec-vms-nm /img/$(basename "$img") 2>/dev/null | grep -iE 'get_meta|alloc_group|free|memset|calloc|enframe' | head -30" 2>&1 || true
done
sec "DONE"
