#!/bin/bash
# TEST: LIBRARY/HELP/CREATE compiles a .HLP into a .HLB and HELP reads topics
#       from the compiled library, located through the HLP$LIBRARY search list.
# EXPECT: contains:LIBRARIAN-S-CREATED
# EXPECT: contains:Directory of help LIBRARY
# EXPECT: contains:ALPHA_BODY_FROM_LIBA
# EXPECT: contains:ALPHA_SUB1_DEEP_TEXT
# EXPECT: contains:BETA_BODY_FROM_LIBB
# EXPECT: contains:SHARED_VARIANT_A
# EXPECT_NOT: contains:SHARED_VARIANT_B
#
# vms-01b (.HLB slice): a VMS HELP library is keyed by its level-1 topics.
# LIBRARY/HELP/CREATE lib.HLB src.HLP compiles the numbered-level .HLP source
# into the key-indexed .HLB (the OVMX "LBRO" container, dcl/hlb.h -- Rule 8),
# and HELP resolves its library through the HLP$LIBRARY, HLP$LIBRARY_1..n search
# list (VSI OpenVMS DCL Dictionary: LIBRARY, HELP). This test proves the whole
# chain: (1) compile produces a real indexed library (LIBRARY/LIST shows its
# modules); (2) HELP ALPHA reads a topic ONLY present in the compiled LIBA.HLB,
# reached via HLP$LIBRARY; (3) HELP BETA reads LIBB.HLB reached via
# HLP$LIBRARY_1, proving the search continues past the first library; (4) a key
# in BOTH libraries (SHARED) resolves to the FIRST library in search order.
VMSDCL="${VMSDCL:-vmsdcl}"

# HELP consults the $OVMX_HELPLIB locator override before the HLP$LIBRARY search
# list; the harness sets it for the .HLP content test. Unset it here so the
# search-list path under test is the one that resolves.
unset OVMX_HELPLIB

TDIR="dcl_test_hlb_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"

# Library A source: ALPHA (with a subtopic) + SHARED (variant A).
cat > "/vms/$TDIR/srca.hlp" <<'EOF'
1 ALPHA
 ALPHA_BODY_FROM_LIBA
2 SUB1
 ALPHA_SUB1_DEEP_TEXT
1 SHARED
 SHARED_VARIANT_A
EOF

# Library B source: BETA + SHARED (variant B).
cat > "/vms/$TDIR/srcb.hlp" <<'EOF'
1 BETA
 BETA_BODY_FROM_LIBB
1 SHARED
 SHARED_VARIANT_B
EOF

# One DCL session: compile both .HLB libraries, list one, wire the HLP$LIBRARY
# search list to them, then drive HELP.
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]
LIBRARY/HELP/CREATE LIBA.HLB SRCA.HLP
LIBRARY/HELP/CREATE LIBB.HLB SRCB.HLP
LIBRARY/LIST LIBA.HLB
DEFINE HLP$LIBRARY SYS$SYSDEVICE:[%s]LIBA
DEFINE HLP$LIBRARY_1 SYS$SYSDEVICE:[%s]LIBB
HELP ALPHA
HELP ALPHA SUB1
HELP BETA
HELP SHARED
' "$VDIR" "$VDIR" "$VDIR" | $VMSDCL 2>&1

rm -rf "/vms/$TDIR"
