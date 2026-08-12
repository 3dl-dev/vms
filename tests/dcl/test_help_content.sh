#!/bin/bash
# TEST: HELP enumerates the real command table and gives topic-specific
# content, not a static banner
# EXPECT: contains:COPY
# EXPECT: contains:DELETE
# EXPECT: contains:SHOW
# EXPECT: contains:HELP_TOPIC_DIFFERENTIATED
# EXPECT_NOT: contains:HELP_TOPIC_SAME_AS_GENERIC
# EXPECT_NOT: contains:HELP_UNKNOWN_TOPIC_SILENT
#
# vms-fe21: re-armed. The old test only grepped for three verb names
# (COPY/DELETE/SHOW) in bare `HELP`'s output. src/vmsdcl/dcl_cmd_misc.c's
# cmd_help() DOES generate that listing from the live command-dispatch table
# (dcl_get_verb_table()), so the names were not a hardcoded banner -- but the
# test could not fail if cmd_help were replaced by a stub that printed a
# fixed line containing those same three words regardless of what was asked,
# because it never checked that HELP actually DISPATCHES on its argument.
#
# The added checks are differential and behavior-grounded in cmd_help()'s
# real branches:
#   - `HELP SHOW` must print the SHOW-specific "Subcommands:" block (with
#     real subcommand names DEFAULT/LOGICAL/PROCESS/...) and must NOT print
#     the generic "Information available:" listing -- proving a specific
#     topic actually took a different code path, not the fallback.
#   - `HELP XYZZY_NOT_A_REAL_COMMAND` (a token that is not a DCL verb) must
#     produce the real %DCL-W-NOHELP error the source raises for an unknown
#     topic (dcl_cmd_misc.c: dcl_error("DCL", 0, "NOHELP", ...)), not silent
#     success and not the generic listing either.
# If cmd_help regressed to always emitting the same content, or dropped the
# unknown-topic error, these fail.
VMSDCL="${VMSDCL:-vmsdcl}"
FAILURES=0

generic_output=$(printf 'HELP\n' | $VMSDCL 2>&1)
show_output=$(printf 'HELP SHOW\n' | $VMSDCL 2>&1)
unknown_output=$(printf 'HELP XYZZY_NOT_A_REAL_COMMAND\n' | $VMSDCL 2>&1)

echo "$generic_output"

# HELP SHOW must show the topic-specific subcommand block...
if echo "$show_output" | grep -q "Subcommands:" && \
   echo "$show_output" | grep -q "DEFAULT.*Display current default directory"; then
    :
else
    echo "  MISSING topic-specific content in HELP SHOW"
    FAILURES=$((FAILURES + 1))
fi

# ...and must NOT be indistinguishable from the generic "list everything" reply.
if echo "$show_output" | grep -q "^Information available:"; then
    echo "HELP_TOPIC_SAME_AS_GENERIC"
    FAILURES=$((FAILURES + 1))
fi

# HELP on an unrecognized topic must produce the real DCL error, not silence
# and not the generic listing (both would mean HELP isn't really dispatching
# on its argument).
if echo "$unknown_output" | grep -qE '%DCL-W-NOHELP'; then
    :
elif [ -z "$(echo "$unknown_output" | tr -d '[:space:]')" ]; then
    echo "HELP_UNKNOWN_TOPIC_SILENT"
    FAILURES=$((FAILURES + 1))
else
    echo "  Unexpected HELP-unknown-topic output: $unknown_output"
    FAILURES=$((FAILURES + 1))
fi
if echo "$unknown_output" | grep -q "^Information available:"; then
    echo "HELP_TOPIC_SAME_AS_GENERIC"
    FAILURES=$((FAILURES + 1))
fi

if [ $FAILURES -eq 0 ]; then
    echo "HELP_TOPIC_DIFFERENTIATED"
fi
