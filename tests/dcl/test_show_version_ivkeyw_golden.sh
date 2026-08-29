#!/bin/bash
# TEST: SHOW VERSION / SHOW VERIFY / SH VER / SH VERI reject as unrecognized
#       SHOW keywords, byte-for-byte identical to real OpenVMS.
#
# Oracle-driven UX-fidelity gate (epic vms-050). OpenVMS has NO `SHOW VERSION`
# and NO `SHOW VERIFY` keyword -- captured verbatim on OpenVMS VAX V7.3 (lab-2)
# and Alpha V8.4 (lab-Alpha), 2026-08-29; the two architectures agree. Each
# produces the two-line message:
#
#     %DCL-W-IVKEYW, unrecognized keyword - check validity and spelling
#      \VERSION\
#
# This diffs OVMX's DCL output against tests/qemu/golden/show_version_ivkeyw.
# golden (the oracle ground truth) byte-for-byte. It fails if OVMX regresses to
# `-E-` severity, to the old "unrecognized SHOW keyword" text, to a single-line
# form, to a non-upcased keyword, OR if a fabricated `SHOW VERSION`/`SHOW
# VERIFY` handler is re-added (which would print something other than IVKEYW).
#
# EXPECT: contains:GOLDEN_MATCH
# EXPECT_NOT: contains:GOLDEN_MISMATCH
# EXPECT: contains:%DCL-W-IVKEYW
# EXPECT_NOT: contains:%DCL-E-IVKEYW
# EXPECT_NOT: contains:unrecognized SHOW keyword
# EXPECT_NOT: contains:VERIFY = O
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GOLDEN="$SCRIPT_DIR/../qemu/golden/show_version_ivkeyw.golden"

if [ ! -f "$GOLDEN" ]; then
    echo "GOLDEN_MISMATCH: golden file not found at $GOLDEN"
    exit 1
fi

# Fixed input: uppercase forms + one lowercase form (proves keyword upcasing).
actual="$(printf 'SHOW VERSION\nSH VER\nSHOW VERIFY\nshow version\n' | $VMSDCL 2>&1)"

# Byte-for-byte compare against the oracle golden.
if diff -u "$GOLDEN" <(printf '%s\n' "$actual") >/tmp/show_version_golden.diff 2>&1; then
    echo "GOLDEN_MATCH: OVMX SHOW VERSION/VERIFY == OpenVMS oracle (VAX V7.3 / Alpha V8.4)"
else
    echo "GOLDEN_MISMATCH: OVMX diverges from the OpenVMS oracle golden:"
    cat /tmp/show_version_golden.diff
fi

# Echo actual so the harness's own %FAC-SEV-IDENT / EXPECT_NOT checks see it.
printf '%s\n' "$actual"
