#!/bin/bash
# TEST: LIBRARY on a .A file is an OBJECT library (the GCC port's ar wrapper, vms-8ca)
# EXPECT: contains:DOTA-AR-ORACLE-OK
# EXPECT: contains:DOTA-REPLACE-ONE-MEMBER-OK
# EXPECT: contains:DOTA-TEXT-LIBRARY-UNCHANGED
#
# The alpha-dec-vms GCC port's `ar` (gcc/config/vms/vms-ar.c) writes a command file
#   $ library /replace/create libx.a -
#   a.o,-
#   b.o
# and runs it. The file is named .A, so DCL must treat it as an OBJECT library (an
# `ar` container of the .OBJ members, which LINK.EXE searches), not as a TEXT library.
# Oracle: the system `ar` reads what DCL wrote. Negative control: a .TLB stays a TEXT
# library (not an `ar` container); replacing a module that is already in does not
# add a second copy.
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_dota_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"
CF="-O2 -ffreestanding -fno-builtin -fno-stack-protector -c"
case "$(uname -m)" in x86_64|amd64) CF="$CF -fcf-protection=none" ;; esac
gcc $CF -x c - -o "/vms/$TDIR/AMOD.OBJ" <<<'int amod_fn(void){return 1;}'
gcc $CF -x c - -o "/vms/$TDIR/BMOD.OBJ" <<<'int bmod_fn(void){return 2;}'

printf 'SET DEFAULT SYS$SYSDEVICE:[%s]
LIBRARY /REPLACE/CREATE LIBX.A AMOD.OBJ
LIBRARY /REPLACE LIBX.A BMOD.OBJ
LIBRARY /REPLACE LIBX.A AMOD.OBJ
LIBRARY /CREATE NOTES.TLB
' "$VDIR" | $VMSDCL 2>&1

LIB=$(find "/vms/$TDIR" -iname 'LIBX.A' 2>/dev/null | head -1)
if [ -n "$LIB" ] && ar t "$LIB" 2>/dev/null | grep -qi AMOD && ar t "$LIB" 2>/dev/null | grep -qi BMOD; then
    echo "DOTA-AR-ORACLE-OK"
else
    echo "DOTA-AR-ORACLE-FAIL: no ar container holding AMOD and BMOD under /vms/$TDIR"
fi
if [ -n "$LIB" ] && [ "$(ar t "$LIB" 2>/dev/null | grep -ci AMOD)" = 1 ]; then
    echo "DOTA-REPLACE-ONE-MEMBER-OK"
else
    echo "DOTA-REPLACE-ONE-MEMBER-FAIL"
fi
TLB=$(find "/vms/$TDIR" -iname 'NOTES.TLB' 2>/dev/null | head -1)
if [ -n "$TLB" ] && ! head -c 7 "$TLB" | grep -q '!<arch>'; then
    echo "DOTA-TEXT-LIBRARY-UNCHANGED"
else
    echo "DOTA-TEXT-LIBRARY-CHANGED"
fi
rm -rf "/vms/$TDIR"
