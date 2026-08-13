#!/bin/bash
# TEST: HELP walks the hierarchical help library (real topic tree), not a shim
# EXPECT: contains:COPY
# EXPECT: contains:DELETE
# EXPECT: contains:SHOW
# EXPECT: contains:HELP_TOPLEVEL_LISTS
# EXPECT: contains:HELP_TOPIC_TEXT_OK
# EXPECT: contains:HELP_SUBTOPIC_LISTING_OK
# EXPECT: contains:HELP_DEEP_SUBTOPIC_OK
# EXPECT: contains:HELP_UNKNOWN_AUTHENTIC
# EXPECT: contains:HELP_TOPIC_DIFFERENTIATED
# EXPECT_NOT: contains:HELP_TOPIC_SAME_AS_GENERIC
# EXPECT_NOT: contains:HELP_UNKNOWN_TOPIC_SILENT
# EXPECT_NOT: regex:%DCL-W-NOHELP
#
# vms-01b: the old cmd_help was a printf shim (per-verb one-liner + three
# hardcoded SHOW/SET/DIRECTORY blocks + a fake one-shot "Topic?"). It is now a
# real reader (src/vmsdcl/dcl_help.c) that walks the topic tree parsed from the
# HELP library data (distro/rootfs/.../SYSHLP/HELPLIB.HLP), located for this
# test via $OVMX_HELPLIB (set by tests/dcl/CMakeLists.txt). These checks are
# behaviour-grounded in the real library content and would fail if HELP
# regressed to a static banner or dropped the authentic not-found message.
VMSDCL="${VMSDCL:-vmsdcl}"
FAILURES=0

toplevel_output=$(printf 'HELP\n' | $VMSDCL 2>&1)
copy_output=$(printf 'HELP COPY\n' | $VMSDCL 2>&1)
show_output=$(printf 'HELP SHOW\n' | $VMSDCL 2>&1)
show_default_output=$(printf 'HELP SHOW DEFAULT\n' | $VMSDCL 2>&1)
unknown_output=$(printf 'HELP XYZZY_NOT_A_REAL_COMMAND\n' | $VMSDCL 2>&1)

# The command names must appear (top-level topic listing).
echo "$toplevel_output"

# Bare HELP lists the top-level topics under the authentic header.
if echo "$toplevel_output" | grep -q "Information available:"; then
    echo "HELP_TOPLEVEL_LISTS"
else
    echo "  MISSING top-level 'Information available:' listing"
    FAILURES=$((FAILURES + 1))
fi

# HELP COPY shows the topic's own body text (from the library, not a banner).
if echo "$copy_output" | grep -qi "Copies one or more files"; then
    echo "HELP_TOPIC_TEXT_OK"
else
    echo "  MISSING topic body text for HELP COPY"
    FAILURES=$((FAILURES + 1))
fi

# HELP SHOW shows the "Additional information available:" subtopic listing with
# real subtopics from the library (DEFAULT, DEVICE) ...
if echo "$show_output" | grep -q "Additional information available:" && \
   echo "$show_output" | grep -q "DEFAULT" && \
   echo "$show_output" | grep -q "DEVICE"; then
    echo "HELP_SUBTOPIC_LISTING_OK"
else
    echo "  MISSING subtopic listing for HELP SHOW"
    FAILURES=$((FAILURES + 1))
fi

# ... and must NOT be indistinguishable from the generic top-level reply.
if echo "$show_output" | grep -q "Information available:"; then
    echo "HELP_TOPIC_SAME_AS_GENERIC"
    FAILURES=$((FAILURES + 1))
fi

# HELP topic subtopic descends two levels into the tree.
if echo "$show_default_output" | grep -qi "current default directory"; then
    echo "HELP_DEEP_SUBTOPIC_OK"
else
    echo "  MISSING deep-subtopic text for HELP SHOW DEFAULT"
    FAILURES=$((FAILURES + 1))
fi

# Unknown topic: authentic "Sorry, no documentation on <x>" (NOT the old,
# inauthentic %DCL-W-NOHELP, and NOT silence, and NOT the generic listing).
if echo "$unknown_output" | grep -q "Sorry, no documentation on XYZZY_NOT_A_REAL_COMMAND"; then
    echo "HELP_UNKNOWN_AUTHENTIC"
elif [ -z "$(echo "$unknown_output" | tr -d '[:space:]')" ]; then
    echo "HELP_UNKNOWN_TOPIC_SILENT"
    FAILURES=$((FAILURES + 1))
else
    echo "  Unexpected HELP-unknown-topic output: $unknown_output"
    FAILURES=$((FAILURES + 1))
fi
if echo "$unknown_output" | grep -q "Information available:"; then
    echo "HELP_TOPIC_SAME_AS_GENERIC"
    FAILURES=$((FAILURES + 1))
fi

if [ $FAILURES -eq 0 ]; then
    echo "HELP_TOPIC_DIFFERENTIATED"
fi
