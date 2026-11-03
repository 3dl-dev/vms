#!/bin/bash
# TEST: LIBRARY/OBJECT (.OLB) creates a REAL ar-container object library (vms-ca9)
# EXPECT: contains:LIBRARIAN-S-CREATED
# EXPECT: contains:OBJMOD
# EXPECT: contains:OLB-AR-ORACLE-OK
#
# Proves dcl_library.c's OBJECT path is a real object library (an `ar` archive of
# .OBJ members that LINK.EXE can consume) — NOT the defined-only "LBRO" stub the
# TEXT/HELP libraries use. The independent oracle is the system `ar`: it must be
# able to read the .OLB DCL wrote and see the inserted module.
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_olb_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"
# LIBRARY writes its libraries through RMS (vms-f8cd), so on the host each is a
# versioned file NAME.TYP;N (with RMS's .rms_meta sidecar): the library is its
# highest version.
vfind() { find "/vms/$TDIR" -iname "$1;*" ! -name '*.rms_meta' 2>/dev/null | sort -t';' -k2 -n | tail -1; }

# A real relocatable object as the library member (name preserved upper-case so
# the DCL default-directory resolution finds it).
CF="-O2 -ffreestanding -fno-builtin -fno-stack-protector -c"
case "$(uname -m)" in x86_64|amd64) CF="$CF -fcf-protection=none" ;; esac
gcc $CF -x c - -o "/vms/$TDIR/OBJMOD.OBJ" <<<'int objmod_fn(void){return 99;}'

printf 'SET DEFAULT SYS$SYSDEVICE:[%s]
LIBRARY /CREATE MYOBJ.OLB OBJMOD.OBJ
LIBRARY /LIST MYOBJ.OLB
' "$VDIR" | $VMSDCL 2>&1

# Independent oracle: the file DCL wrote must be a valid ar archive holding the
# module (the OBJECT path uses the shared ovmx_olb.h ar writer). DCL lower-cases
# the resolved directory/filename, so locate it case-insensitively.
OLB=$(vfind MYOBJ.OLB)
if [ -n "$OLB" ] && ar t "$OLB" 2>/dev/null | grep -qi OBJMOD; then
    echo "OLB-AR-ORACLE-OK"
else
    echo "OLB-AR-ORACLE-FAIL: no ar container with OBJMOD under /vms/$TDIR"
    [ -n "$OLB" ] && head -c 8 "$OLB" | od -c | head -1
fi

rm -rf "/vms/$TDIR"
