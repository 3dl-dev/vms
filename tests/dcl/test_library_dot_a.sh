#!/bin/bash
# TEST: LIBRARY on a .A file is an OBJECT library (the GCC port's ar wrapper, vms-8ca)
# EXPECT: contains:DOTA-AR-ORACLE-OK
# EXPECT: contains:DOTA-REPLACE-ONE-MEMBER-OK
# EXPECT: contains:DOTA-TEXT-LIBRARY-UNCHANGED
# EXPECT: contains:DOTA-LONG-LIST-ALL-10-OK
# EXPECT: contains:DOTA-DELETE-LIST-ALL-9-OK
# EXPECT_NOT: contains:MAXPARM
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
# LIBRARY writes its libraries through RMS (vms-f8cd), so on the host each is a
# versioned file NAME.TYP;N (with RMS's .rms_meta sidecar): the library is its
# highest version.
vfind() { find "/vms/$TDIR" -iname "$1;*" ! -name '*.rms_meta' 2>/dev/null | sort -t';' -k2 -n | tail -1; }
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

# rd vms-465: vms-ar.c puts a whole batch in ONE command; past P8 (the library and
# seven files) the list was silently cut. Ten objects, as its command file writes them.
for i in 0 1 2 3 4 5 6 7 8 9; do
    gcc $CF -x c - -o "/vms/$TDIR/L$i.OBJ" <<<"int long_fn_$i(void){return $i;}"
done
{ printf '$ library /replace/create LONG.A -\n'
  for i in 0 1 2 3 4 5 6 7 8; do printf 'L%s.OBJ,-\n' $i; done
  printf 'L9.OBJ\n'
  printf '$ WRITE SYS$OUTPUT "DOTA-LONG-STATUS=\x27\x27$STATUS\x27"\n'
} > "/vms/$TDIR/ARLONG.COM"
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]
@ARLONG.COM
DELETE L0.OBJ;*,L1.OBJ;*,L2.OBJ;*,L3.OBJ;*,L4.OBJ;*,L5.OBJ;*,L6.OBJ;*,L7.OBJ;*,L8.OBJ;*
' "$VDIR" | $VMSDCL 2>&1
LONG=$(vfind LONG.A)
if [ -n "$LONG" ] && [ "$(ar t "$LONG" 2>/dev/null | grep -ci '^L[0-9]')" = 10 ]; then
    echo "DOTA-LONG-LIST-ALL-10-OK"
else
    echo "DOTA-LONG-LIST-FAIL: $(ar t "$LONG" 2>/dev/null | tr '\n' ' ')"
fi
# rd vms-ba2: DELETE takes a list past P8 too, as OpenVMS does -- all nine go,
# L9 (not in the list) stays
if [ "$(find "/vms/$TDIR" -iname 'L[0-8].OBJ*' | wc -l)" -eq 0 ] &&
   [ "$(find "/vms/$TDIR" -iname 'L9.OBJ*' | wc -l)" -eq 1 ]; then
    echo "DOTA-DELETE-LIST-ALL-9-OK"
else
    echo "DOTA-DELETE-LIST-FAIL: $(find "/vms/$TDIR" -iname 'L[0-9].OBJ*' | tr '\n' ' ')"
fi

LIB=$(vfind LIBX.A)
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
TLB=$(vfind NOTES.TLB)
if [ -n "$TLB" ] && ! head -c 7 "$TLB" | grep -q '!<arch>'; then
    echo "DOTA-TEXT-LIBRARY-UNCHANGED"
else
    echo "DOTA-TEXT-LIBRARY-CHANGED"
fi
rm -rf "/vms/$TDIR"
