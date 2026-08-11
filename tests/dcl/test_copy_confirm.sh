#!/bin/bash
# TEST: COPY qualifier coverage (vms-7543) - /CONFIRM is honoured for real, and
#       an unimplemented COPY qualifier draws the authentic %DCL-W-IVQUAL.
#       Grounded in the public VSI OpenVMS DCL Dictionary COPY entry.
#
# The answer to each /CONFIRM prompt is the next line on the command stream
# (cmd_copy reads the Y/N from stdin, exactly as DELETE/CONFIRM does). We probe
# the OUTCOME with TYPE rather than the prompt text (the prompt echoes the
# destination name, so a name-substring check would be ambiguous):
#   - answered N -> NOPE.TXT was NOT created -> TYPE NOPE.TXT fails with FNF
#   - answered Y -> YEP.TXT WAS created      -> TYPE YEP.TXT prints PAYLOAD
# EXPECT: contains:PAYLOAD
# EXPECT: contains:file not found - NOPE.TXT
#
# --- an unimplemented COPY qualifier -> %DCL-W-IVQUAL (structural, q_copy) ---
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \CONTIGUOUS\
#
# THE FINDING THIS GATES (docs/design-vms-parity-map.md sec 3): COPY carried 1
# of ~20 Dictionary qualifiers and accepted any other silently. TRIPWIRE:
# remove the /CONFIRM handling in cmd_copy() (src/vmsdcl/dcl_cmd_file.c) and the
# N answer no longer suppresses the copy - "file not found - NOPE.TXT"
# disappears; drop the /CONFIRM entry from q_copy (src/vmsdcl/dcl_builtin.c) and
# it is silently accepted again. /CONTIGUOUS is a real VMS COPY qualifier OVMX
# does not implement - honestly rejected (over-restriction), not faked.
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR=$(mktemp -d)
echo "PAYLOAD" > "$TDIR/src.txt"
printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nCOPY/CONFIRM SRC.TXT NOPE.TXT\nN\nCOPY/CONFIRM SRC.TXT YEP.TXT\nY\nTYPE YEP.TXT\nTYPE NOPE.TXT\nCOPY/CONTIGUOUS SRC.TXT CFG.TXT\n' "$TDIR" | $VMSDCL 2>&1
rm -rf "$TDIR"
