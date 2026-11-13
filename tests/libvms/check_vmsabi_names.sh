#!/bin/sh
# check_vmsabi_names.sh - every name src/libvms/include/ovmx_vmsabi_names.h
# binds a 32-bit DEC C caller to is a VMS-ABI entry point that exists: an
# upper-case definition in src/libvms/syssvc/sys_vmsabi.c (vms-7b3). A name
# listed without its entry would be %LINK-F-UNDEF in every 32-bit image.
#
# Usage: check_vmsabi_names.sh <repo-root>      (ctest: check_vmsabi_names)
set -eu
ROOT=$1
H=$ROOT/src/libvms/include/ovmx_vmsabi_names.h
C=$ROOT/src/libvms/syssvc/sys_vmsabi.c
n=0; bad=0
for pair in $(awk '/^#pragma redefine_extname/{print $3 "=" $4}' "$H"); do
    lo=${pair%%=*}; up=${pair##*=}
    n=$((n+1))
    want=$(printf '%s' "$lo" | tr 'a-z' 'A-Z')
    [ "$want" = "$up" ] || { echo "FAIL: $lo maps to $up, not its upper-case name $want"; bad=$((bad+1)); continue; }
    re=$(printf '%s' "$up" | sed 's/\$/\\$/g')
    grep -qE "^int $re\(" "$C" || { echo "FAIL: $up has no VMS-ABI definition in sys_vmsabi.c"; bad=$((bad+1)); }
done
[ "$n" -gt 0 ] || { echo "FAIL: no names read from $H"; exit 1; }
[ "$bad" -eq 0 ] || exit 1
echo "check_vmsabi_names: $n names, each a VMS-ABI entry in sys_vmsabi.c"
