#!/bin/bash
# TEST: SET SYMBOL /ALL /GENERAL /VERB scope-domain selectors + /GLOBAL /LOCAL IVQUAL (vms-c211)
#   OpenVMS DCL Dictionary (VSI OpenVMS DCL Dictionary N-Z, SET SYMBOL): the
#   /SCOPE=([NO]LOCAL,[NO]GLOBAL) change can be scoped to one of two
#   symbol-translation contexts by a domain qualifier:
#     /VERB    - applies ONLY to translation of the first token on a command
#                line as a symbol (verb-position translation)
#     /GENERAL - applies to all symbols EXCEPT the first token
#     /ALL     - (default) applies to BOTH
#   These three are mutually exclusive.
#
#   Differential test. A global FOO=="WRITE" is a verb-position translation
#   target (bare "FOO ..." runs "WRITE ..."); a global BAR=="barval" is read
#   only via general 'BAR' substitution. The SAME two symbols are used in every
#   scenario; only the domain qualifier differs, so any behaviour change is
#   attributable to the qualifier alone:
#     /SCOPE=NOGLOBAL/VERB    -> FOO hidden from verb translation (IVVERB, so
#                               VB_VERB_RAN never prints) but 'BAR' still expands
#                               (VB_GEN=<barval>).
#     /SCOPE=NOGLOBAL/GENERAL -> 'BAR' hidden (GN_GEN=<>) but FOO->WRITE still
#                               runs (GN_VERB_RAN prints).
#     /SCOPE=NOGLOBAL (/ALL)  -> both hidden (AL_GEN=<>, no AL_VERB_RAN).
#
#   /GLOBAL and /LOCAL are NOT SET SYMBOL qualifiers (LOCAL/GLOBAL are /SCOPE
#   keywords). Real DCL rejects them with %DCL-W-IVQUAL; OVMX does the same.
#
# EXPECT: contains:VB_GEN=<barval>
# EXPECT_NOT: contains:VB_VERB_RAN
# EXPECT: contains:GN_VERB_RAN
# EXPECT: contains:GN_GEN=<>
# EXPECT_NOT: contains:GN_GEN=<barval>
# EXPECT: contains:AL_GEN=<>
# EXPECT_NOT: contains:AL_GEN=<barval>
# EXPECT_NOT: contains:AL_VERB_RAN
# EXPECT: contains:%DCL-E-IVVERB
# EXPECT: contains:%DCL-W-IVQUAL
# EXPECT: contains:\GLOBAL\
# EXPECT: contains:\LOCAL\
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"

TDIR="dcl_scopedom_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"

# /VERB: hide global from verb-position translation only. FOO as a verb is
# hidden (IVVERB, VB_VERB_RAN never runs); 'BAR' general substitution works.
cat > "/vms/$TDIR/verb.com" << 'EOF'
$ FOO == "WRITE"
$ BAR == "barval"
$ SET SYMBOL/SCOPE=NOGLOBAL/VERB
$ WRITE SYS$OUTPUT "VB_GEN=<''BAR'>"
$ FOO SYS$OUTPUT "VB_VERB_RAN"
$ EXIT
EOF

# /GENERAL: hide global from general substitution only. 'BAR' expands to
# nothing (GN_GEN=<>); FOO->WRITE verb translation still runs.
cat > "/vms/$TDIR/general.com" << 'EOF'
$ FOO == "WRITE"
$ BAR == "barval"
$ SET SYMBOL/SCOPE=NOGLOBAL/GENERAL
$ FOO SYS$OUTPUT "GN_VERB_RAN"
$ WRITE SYS$OUTPUT "GN_GEN=<''BAR'>"
$ EXIT
EOF

# /ALL (default): hide global from BOTH domains.
cat > "/vms/$TDIR/all.com" << 'EOF'
$ FOO == "WRITE"
$ BAR == "barval"
$ SET SYMBOL/SCOPE=NOGLOBAL
$ WRITE SYS$OUTPUT "AL_GEN=<''BAR'>"
$ FOO SYS$OUTPUT "AL_VERB_RAN"
$ EXIT
EOF

printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\n@verb.com\n@general.com\n@all.com\n' "$VDIR" | $VMSDCL 2>&1

# /GLOBAL and /LOCAL are not real SET SYMBOL qualifiers -> %DCL-W-IVQUAL.
printf 'SET SYMBOL/GLOBAL\nSET SYMBOL/LOCAL\n' | $VMSDCL 2>&1

rm -rf "/vms/$TDIR"
