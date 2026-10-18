#!/bin/bash
# TEST: symbols are stored sized to their value (vms-25f/83d: a fixed 4.4 KB
# entry per symbol held ~2 MB of a 16 MB VAX) -- grow, shrink, self-append,
# long (4096-truncated) values, delete and redefine must all keep VMS semantics.
# EXPECT: contains:SHORT=a
# EXPECT: contains:GROWN=abcdefghijklmnopqrstuvwxyz0123456789
# EXPECT: contains:SHRUNK=z
# EXPECT: contains:APPEND=xyyy
# EXPECT: regex:LONGLEN=[0-9]{3,4}
# EXPECT: contains:REDEF=again
# EXPECT_NOT: contains:%DCL-W-UNDSYM, undefined symbol - X
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
long=$(printf 'q%.0s' $(seq 1 256))
{
cat <<'DCL'
X = "a"
WRITE SYS$OUTPUT "SHORT=''X'"
X = "abcdefghijklmnopqrstuvwxyz0123456789"
WRITE SYS$OUTPUT "GROWN=''X'"
X = "z"
WRITE SYS$OUTPUT "SHRUNK=''X'"
X = "x"
X = X + "y" + "y" + "y"
WRITE SYS$OUTPUT "APPEND=''X'"
DCL
echo "L = \"$long\""
cat <<'DCL'
L = L + L
L = L + L
L = L + L
L = L + L
L = L + L
LL = F$LENGTH(L)
WRITE SYS$OUTPUT "LONGLEN=''LL'"
DELETE/SYMBOL X
X = "again"
WRITE SYS$OUTPUT "REDEF=''X'"
DCL
} | $VMSDCL 2>&1
