#!/bin/bash
# TEST: HELP <verb> surfaces per-command qualifiers from the Engine A CDU
#       command tables, in sync with the actually-accepted syntax (vms-01b).
# EXPECT: contains:CDU_DIR_EXCLUDE_LISTED
# EXPECT: contains:CDU_DIR_HEADING_LISTED
# EXPECT: contains:CDU_DIR_KEPT_BRIEF
# EXPECT: contains:CDU_DIR_NO_OUTPUT
# EXPECT: contains:CDU_DIR_EXCLUDE_FORMAT
# EXPECT: contains:CDU_DIR_DATE_KEYWORD
# EXPECT: contains:CDU_COPY_NEWVERSION_LISTED
# EXPECT: contains:CDU_APPEND_NO_QUALS
# EXPECT: contains:CDU_UNKNOWN_AUTHENTIC
# EXPECT_NOT: contains:CDU_DIR_OUTPUT_LEAK
# EXPECT_NOT: contains:CDU_APPEND_QUALS_LEAK
#
# vms-01b (Engine A CDU-table help slice): the qualifier information HELP shows
# for a verb backed by a command definition is now derived from the live CDU
# qualifier tables (src/vmsdcl/dcl_builtin.c: struct dcl_verb.quals), not a
# hand-maintained HELPLIB string. The staged HELPLIB.HLP had DRIFTED from the
# parser: its DIRECTORY "Qualifiers" listed /OUTPUT (which the parser rejects
# with %DCL-W-IVQUAL) and omitted /EXCLUDE, /HEADING, /TRAILING, /GRAND_TOTAL
# (which the parser accepts). After folding the CDU table in, HELP lists exactly
# the accepted qualifiers. Verbs that honour NO qualifier (APPEND -- explicit
# empty CDU table) no longer advertise a stale "Qualifiers" subtopic. HELPLIB is
# reached via $OVMX_HELPLIB (set by tests/dcl/CMakeLists.txt).
VMSDCL="${VMSDCL:-vmsdcl}"
FAILURES=0

dir_quals=$(printf 'HELP DIRECTORY QUALIFIERS\n' | $VMSDCL 2>&1)
dir_exclude=$(printf 'HELP DIRECTORY QUALIFIERS /EXCLUDE\n' | $VMSDCL 2>&1)
dir_date=$(printf 'HELP DIRECTORY QUALIFIERS /DATE\n' | $VMSDCL 2>&1)
copy_quals=$(printf 'HELP COPY QUALIFIERS\n' | $VMSDCL 2>&1)
append_top=$(printf 'HELP APPEND\n' | $VMSDCL 2>&1)
append_quals=$(printf 'HELP APPEND QUALIFIERS\n' | $VMSDCL 2>&1)

# 1. DIRECTORY qualifiers the CDU table accepts but HELPLIB omitted are now
#    listed (proves the listing is CDU-driven, not the hand-authored string).
if echo "$dir_quals" | grep -q "/EXCLUDE"; then
    echo "CDU_DIR_EXCLUDE_LISTED"
else
    echo "  MISSING /EXCLUDE in HELP DIRECTORY QUALIFIERS"; FAILURES=$((FAILURES+1))
fi
if echo "$dir_quals" | grep -q "/HEADING" && \
   echo "$dir_quals" | grep -q "/TRAILING" && \
   echo "$dir_quals" | grep -q "/GRAND_TOTAL"; then
    echo "CDU_DIR_HEADING_LISTED"
else
    echo "  MISSING /HEADING|/TRAILING|/GRAND_TOTAL"; FAILURES=$((FAILURES+1))
fi

# 2. Qualifiers both sources agree on are still present.
if echo "$dir_quals" | grep -q "/BRIEF" && echo "$dir_quals" | grep -q "/FULL"; then
    echo "CDU_DIR_KEPT_BRIEF"
else
    echo "  MISSING /BRIEF|/FULL"; FAILURES=$((FAILURES+1))
fi

# 3. /OUTPUT -- a HELPLIB entry the parser does NOT accept -- is gone (the drift
#    lie removed). Emit a distinct sentinel token if the leak is detected.
if echo "$dir_quals" | grep -q "/OUTPUT"; then
    echo "CDU_DIR_OUTPUT_LEAK"; FAILURES=$((FAILURES+1))
else
    echo "CDU_DIR_NO_OUTPUT"
fi

# 4. Drilling into a qualifier shows its CDU-derived accepted-syntax line,
#    rendered from the table's value-type. /EXCLUDE is CDU_VT_VALUE + VALREQ ->
#    "/EXCLUDE=value"; a HELP qualifier token is folded onto the topic path
#    (VMS-faithful) rather than drawing %DCL-W-IVQUAL.
if echo "$dir_exclude" | grep -q "Format: /EXCLUDE=value"; then
    echo "CDU_DIR_EXCLUDE_FORMAT"
else
    echo "  MISSING CDU format line for /EXCLUDE"; FAILURES=$((FAILURES+1))
fi

#    /DATE is CDU_VT_KEYWORD + NEGATABLE with a default -> "/[NO]DATE=keyword",
#    plus the keyword set and default, all derived from the CDU table.
if echo "$dir_date" | grep -q "Format: /\[NO\]DATE=keyword" && \
   echo "$dir_date" | grep -q "Keywords: MODIFIED" && \
   echo "$dir_date" | grep -q "Default: MODIFIED"; then
    echo "CDU_DIR_DATE_KEYWORD"
else
    echo "  MISSING CDU keyword/default detail for /DATE"; FAILURES=$((FAILURES+1))
fi

# 5. COPY qualifiers come from q_copy (LOG, CONFIRM, NEW_VERSION).
if echo "$copy_quals" | grep -q "/NEW_VERSION" && \
   echo "$copy_quals" | grep -q "/CONFIRM"; then
    echo "CDU_COPY_NEWVERSION_LISTED"
else
    echo "  MISSING /NEW_VERSION|/CONFIRM in HELP COPY QUALIFIERS"; FAILURES=$((FAILURES+1))
fi

# 6. APPEND honours NO qualifier (explicit empty CDU table): its stale HELPLIB
#    "Qualifiers" subtopic is dropped -- HELP APPEND no longer advertises it,
#    and HELP APPEND QUALIFIERS returns the authentic not-found message.
if echo "$append_top" | grep -q "Additional information available:" && \
   echo "$append_top" | grep -qw "Qualifiers"; then
    echo "CDU_APPEND_QUALS_LEAK"; FAILURES=$((FAILURES+1))
elif echo "$append_quals" | grep -q "Sorry, no documentation on APPEND QUALIFIERS"; then
    echo "CDU_APPEND_NO_QUALS"
else
    echo "  APPEND qualifier removal not authentic"; FAILURES=$((FAILURES+1))
fi

# 7. A verb with no command definition still behaves authentically (no fake).
unknown=$(printf 'HELP XYZZY_NOT_A_REAL_COMMAND\n' | $VMSDCL 2>&1)
if echo "$unknown" | grep -q "Sorry, no documentation on XYZZY_NOT_A_REAL_COMMAND"; then
    echo "CDU_UNKNOWN_AUTHENTIC"
else
    echo "  MISSING authentic not-found for unknown verb"; FAILURES=$((FAILURES+1))
fi

if [ "$FAILURES" -eq 0 ]; then
    echo "HELP_CDU_QUALIFIERS_OK"
    exit 0
fi
echo "HELP_CDU_QUALIFIERS_FAILURES=$FAILURES"
exit 1
