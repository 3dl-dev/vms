#!/bin/bash
# TEST: F$GETSYI("SCSNODE") reads the configured cluster node identity,
# distinct from F$GETSYI("NODENAME") (Linux hostname) (vms-ci.8)
# EXPECT: contains:NODE = "TESTND"
# EXPECT: contains:SID = 4242
# EXPECT: contains:SCSNODE_DIFFERS_FROM_NODENAME
# EXPECT_NOT: contains:SCSNODE_SAME_AS_NODENAME
# EXPECT_NOT: contains:%DCL-
VMSDCL="${VMSDCL:-vmsdcl}"
SYSGEN="${SYSGEN:-$(dirname "$VMSDCL")/SYSGEN.EXE}"

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
SYSPARAMS="$TMPDIR/sysparams.dat"

if [ -x "$SYSGEN" ]; then
    # SYSGEN's WRITE <filename> (as opposed to WRITE CURRENT) writes wherever
    # asked, no /vms mount needed, matching sysgen_current_path()'s
    # OVMX_SYSGEN_PATH override used by the readers below (vms-d34).
    printf 'USE DEFAULT\nSET SCSNODE testnd\nSET SCSSYSTEMID 4242\nWRITE %s\nEXIT\n' \
        "$SYSPARAMS" | "$SYSGEN" >/dev/null 2>&1
fi

export OVMX_SYSGEN_PATH="$SYSPARAMS"

printf 'NODE = F$GETSYI("SCSNODE")\nHOST = F$GETSYI("NODENAME")\nSID = F$GETSYI("SCSSYSTEMID")\nSHOW SYMBOL NODE\nSHOW SYMBOL HOST\nSHOW SYMBOL SID\nIF NODE .EQS. HOST THEN WRITE SYS$OUTPUT "SCSNODE_SAME_AS_NODENAME"\nIF NODE .NES. HOST THEN WRITE SYS$OUTPUT "SCSNODE_DIFFERS_FROM_NODENAME"\n' \
    | $VMSDCL 2>&1
