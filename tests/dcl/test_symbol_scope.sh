#!/bin/bash
# TEST: Per-@-level local symbol scope (vms-2af)
#   VMS gives each command-procedure level its own local symbol table:
#     - an inner procedure can READ an outer level's local symbols
#     - an assignment in an inner procedure creates a local at that level and
#       does NOT modify a same-named local in the caller (write is local)
#     - a level's local symbols are discarded on return (do not leak to caller)
#     - GLOBAL symbols (==) are shared across all levels
#     - SET SYMBOL/SCOPE=NOLOCAL hides outer-level locals from the current level
#   Refs: OpenVMS User's Manual (command-level symbol scope); OpenVMS DCL
#   Dictionary, SET SYMBOL /SCOPE=([NO]LOCAL,[NO]GLOBAL).
#
# EXPECT: contains:INNER_SEES_L1=l1val
# EXPECT: contains:INNER_L2=l2val
# EXPECT: contains:OUTER_L1_AFTER=l1val
# EXPECT: contains:OUTER_L2_MARK=<>
# EXPECT_NOT: contains:OUTER_L2_MARK=<l2val>
# EXPECT: contains:OUTER_G=g_from_inner
# EXPECT: contains:NOLOCAL_L1_MARK=<>
# EXPECT_NOT: contains:NOLOCAL_L1_MARK=<l1val>
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"

TDIR="dcl_scope_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"

# Inner procedure: reads caller's L1, defines its own L2, tries to clobber L1
# (which must create a NEW local at THIS level, not modify the caller's), and
# sets a global that the caller must see afterwards.
cat > "/vms/$TDIR/inner.com" << 'EOF'
$ WRITE SYS$OUTPUT "INNER_SEES_L1=''L1'"
$ L2 = "l2val"
$ WRITE SYS$OUTPUT "INNER_L2=''L2'"
$ L1 = "clobbered_by_inner"
$ G == "g_from_inner"
$ EXIT
EOF

# Inner procedure run under SET SYMBOL/SCOPE=NOLOCAL: must NOT see the caller's
# local L1.
cat > "/vms/$TDIR/inner_nolocal.com" << 'EOF'
$ SET SYMBOL/SCOPE=NOLOCAL
$ WRITE SYS$OUTPUT "NOLOCAL_L1_MARK=<''L1'>"
$ EXIT
EOF

# Outer procedure: defines L1, calls inner, then verifies scoping.
cat > "/vms/$TDIR/outer.com" << 'EOF'
$ L1 = "l1val"
$ G == "g_from_outer"
$ @inner.com
$ WRITE SYS$OUTPUT "OUTER_L1_AFTER=''L1'"
$ WRITE SYS$OUTPUT "OUTER_L2_MARK=<''L2'>"
$ WRITE SYS$OUTPUT "OUTER_G=''G'"
$ @inner_nolocal.com
$ EXIT
EOF

printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\n@outer.com\n' "$VDIR" | $VMSDCL 2>&1

rm -rf "/vms/$TDIR"
