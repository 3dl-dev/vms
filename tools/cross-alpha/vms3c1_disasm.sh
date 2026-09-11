#!/usr/bin/env bash
# vms3c1_disasm.sh - focused codegen evidence for vms-3c1 (DIAGNOSTIC).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; REPO="$(cd "$HERE/../.." && pwd)"; cd "$REPO"
sec(){ echo; echo "#### $* ####"; }
docker build -t ovmx-cross-alpha-vms tools/cross-alpha-vms >/tmp/img.log 2>&1 || { echo IMG-FAIL; tail -5 /tmp/img.log; }

sec "COMPILER TYPE WIDTHS (LLP64)"
docker run --rm ovmx-cross-alpha-vms bash -c '
export PATH=/opt/cross-alpha-vms/bin:$PATH
for mp in "" "-mpointer-size=64"; do echo "== flags: [$mp] =="
 alpha-dec-vms-gcc $mp -dM -E -x c /dev/null 2>/dev/null | grep -iE "__SIZEOF_(SIZE_T|POINTER|PTRDIFF_T|LONG|INT|LONG_LONG)__|__SIZE_TYPE__|__PTRDIFF_TYPE__|__INTPTR_TYPE__|__LP64__|__POINTER_WIDTH__" | sort
done'

sec "BUILD veneer DECC\$SHR + disassemble mallocng free/get_meta path"
docker run --rm -v "$REPO":/src ovmx-cross-alpha-vms bash -c '
set -uo pipefail
export PATH=/opt/cross-alpha-vms/bin:$PATH
export IMG=ovmx-cross-alpha-vms
OUT=/tmp/dv; WORKROOT=/tmp/dvwork
mkdir -p "$OUT"
# build-decc-veneer builds musl (obj/*.o) + links DECC$SHR into $OUT
bash /src/tools/cross-alpha-vms/decc-veneer/build-decc-veneer.sh "$OUT" >/tmp/dv.log 2>&1
echo "decc-veneer build rc=$?"; tail -4 /tmp/dv.log
echo "=== locate artifacts ==="
find /tmp -maxdepth 8 \( -path "*mallocng/malloc.o" -o -path "*mallocng/free.o" -o -iname "DECC\$SHR.EXE" \) 2>/dev/null | head
MO=$(find /tmp -maxdepth 8 -path "*mallocng/malloc.o" 2>/dev/null | head -1)
FO=$(find /tmp -maxdepth 8 -path "*mallocng/free.o" 2>/dev/null | head -1)
DSHR=$(find /tmp -maxdepth 8 -iname "DECC\$SHR.EXE" 2>/dev/null | head -1)
echo "MO=$MO FO=$FO DSHR=$DSHR"
for o in "$FO" "$MO"; do [ -f "$o" ] || continue
  echo "############ nm $o ############"; alpha-dec-vms-nm "$o" 2>/dev/null | head -40
  echo "############ objdump -dr $o (FULL, get_meta inlined into free/free_group) ############"
  alpha-dec-vms-objdump -dr "$o" 2>/dev/null
done
if [ -n "$DSHR" ] && [ -f "$DSHR" ]; then
  echo "############ DECC\$SHR nm (free/malloc/meta) ############"
  alpha-dec-vms-nm "$DSHR" 2>/dev/null | grep -iE "free|malloc|meta|group|calloc|alloc_slot|try_avail|enframe" | head -40
  echo "############ DECC\$SHR objdump window around fault 0x4a2dc / 0x1e2dc / 0x4a354 ############"
  alpha-dec-vms-objdump -d "$DSHR" 2>/dev/null > /tmp/dshr.dis
  echo "-- total dis lines: $(wc -l </tmp/dshr.dis) --"
  for off in 4a2dc 4a98c 4a354 1e2dc 1e98c; do
    echo "==== window @ $off ===="
    grep -nE "^ *[0-9a-f]* *$off:|   $off:" /tmp/dshr.dis | head -2
    awk -v t="$off" "BEGIN{f=0} /:/{ split(\$1,a,\":\"); if(a[1]==t){f=1} } f{print; n++; if(n>40) exit}" /tmp/dshr.dis
  done
fi
'
sec DONE
