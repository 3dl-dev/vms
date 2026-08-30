#!/usr/bin/env bash
#
# diff_surface.sh (rd vms-d008) -- compare OVMX's output for a named surface
# against the oracle golden and CLASSIFY the divergence. The OVMX-side complement
# to capture_oracle.sh: it lets vms-c38 turn the hand-written DCL/SHOW acceptance
# assertions (tests/qemu/lib/dcl_acceptance_battery.sh must_have '...') into a
# CONTINUOUS golden-diff gate.
#
# It applies the SAME surface NORMALIZE mask (via `capture_oracle.sh normalize`,
# so the oracle and OVMX are masked identically -- a volatile field can never red
# the diff), then classifies OVMX's output as exactly one of:
#
#   MATCH            normalized OVMX == golden (the surface is VMS-faithful)
#   MISSING          OVMX does not implement the surface -- %DCL-W-IVVERB/IVKEYW/
#                    NOCMD/ABVERB, SS$_UNSUPPORTED, "not implemented", or an
#                    echo-then-error with no data body
#   HOLLOW           OVMX runs it but emits NO data where the oracle has data --
#                    the INV-6 lie-of-absence (empty/blank body under a real header)
#   ARTIFICE-TELL    OVMX output matches the surface's declared fabrication
#                    signature (optional ARTIFICE_TELL='regex' in the .surface)
#   FORMAT-DIVERGENT normalized OVMX differs structurally from the golden
#                    (real data, wrong shape) -- the diff is emitted
#
# The classification order is most-damning-first: MISSING/HOLLOW/ARTIFICE-TELL are
# INV-6 tells that a hand-written must_have could pass vacuously on; only then
# MATCH vs FORMAT-DIVERGENT.
#
# USAGE:
#   diff_surface.sh <surface> [--input FILE]   # OVMX output on stdin (or FILE)
#   diff_surface.sh <surface> --golden-self    # sanity: golden vs itself -> MATCH
#   diff_surface.sh selftest                   # classifier can-fail proof, no lab
#
# EXIT: 0 MATCH; 2 MISSING; 3 HOLLOW; 4 ARTIFICE-TELL; 5 FORMAT-DIVERGENT;
#       1 usage/no-golden. (A gate treats 0 as pass; 2/3/5 as real divergence;
#       4 as a fabrication to excise; the report names which.)
#
# No `set -e` -- classification leans on grep exit codes in conditionals.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SURF_DIR="$HERE/surfaces"
GOLDEN_DIR="$REPO/docs/oracle/golden"
CAPTURE="$HERE/capture_oracle.sh"

die() { echo "diff_surface: $*" >&2; exit 1; }

# body_of -- strip the command ECHO lines ("$ CMD"), bare prompts and blank lines,
# leaving the DATA the command produced. Used to tell "ran but empty" (HOLLOW)
# from "produced data".
body_of() { grep -vE '^\$ |^\$[[:space:]]*$|^[[:space:]]*$' || true; }

# MISSING signatures: OVMX rejected/never-implemented the command.
MISSING_RE='%DCL-[WEF]-(IVVERB|IVKEYW|NOCMD|ABVERB|IVQUAL|SYNTAX)|%SYSTEM-[WEF]-(UNSUPPORTED|NOSUCHDEV)|-RMS-[EF]-|[Nn]ot implemented|SS\$_UNSUPPORTED'

# classify <surface> <ovmx_file> <golden_file> <normalize_cmd...>
# echoes the classification report to stdout, returns the class exit code.
classify() {
    local surface="$1" ovmx="$2" golden="$3"; shift 3
    local artifice="${ARTIFICE_TELL:-}"
    local ovmx_body golden_body ovmx_norm

    ovmx_body="$(body_of < "$ovmx")"
    # ovmx_real = the data body with the not-implemented error lines ALSO removed,
    # so an "echo + %DCL-W-IVVERB" (a tell, not data) reads as no-real-data.
    local ovmx_real; ovmx_real="$(body_of < "$ovmx" | grep -vE "$MISSING_RE" || true)"
    golden_body="$(grep -vE '^\$ |^\$[[:space:]]*$|^[[:space:]]*$' < "$golden" || true)"

    # 1. MISSING -- a not-implemented tell and no real (non-error) data body.
    if grep -qE "$MISSING_RE" "$ovmx" && [ -z "$ovmx_real" ]; then
        echo "MISSING: OVMX does not implement '$surface' (rejected/not-implemented, no data)."
        grep -E "$MISSING_RE" "$ovmx" | sed 's/^/    /' | head -3
        return 2
    fi

    # 2. HOLLOW -- OVMX ran it (no error tell) but produced no data where the
    #    oracle has data. INV-6 lie-of-absence.
    if [ -z "$ovmx_body" ] && [ -n "$golden_body" ]; then
        echo "HOLLOW: '$surface' produced NO data body, but the oracle golden has $(printf '%s\n' "$golden_body" | grep -c . ) data line(s) (INV-6 lie-of-absence)."
        return 3
    fi

    # 3. ARTIFICE-TELL -- a declared fabrication signature.
    if [ -n "$artifice" ] && grep -qE "$artifice" "$ovmx"; then
        echo "ARTIFICE-TELL: '$surface' matches its declared fabrication signature /$artifice/ -- excise the fake (INV-6)."
        grep -E "$artifice" "$ovmx" | sed 's/^/    /' | head -3
        return 4
    fi

    # 4/5. MATCH vs FORMAT-DIVERGENT -- normalize both sides symmetrically.
    ovmx_norm="$("$@" < "$ovmx")"
    if [ "$ovmx_norm" = "$(cat "$golden")" ]; then
        echo "MATCH: '$surface' is VMS-faithful (normalized OVMX == golden)."
        return 0
    fi

    echo "FORMAT-DIVERGENT: '$surface' has data but the wrong shape (normalized diff, golden < vs OVMX >):"
    diff <(cat "$golden") <(printf '%s\n' "$ovmx_norm") | sed 's/^/    /' | head -40
    return 5
}

selftest() {
    echo "=== diff_surface selftest: each class fires on a crafted fixture ==="
    local fails=0 tmp; tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' RETURN

    # A golden with a header + one data row, digit-masked (mirrors a real surface).
    printf '%s\n' '$ SHOW MEMORY' '  Main Memory   ######   ######' > "$tmp/golden"
    local NORM=(sed 's/[0-9]/#/g')   # the surface mask

    expect() { # <desc> <want-rc> <ovmx-fixture-file> [ARTIFICE_TELL]
        local desc="$1" want="$2" f="$3"; ARTIFICE_TELL="${4:-}"
        local out rc
        out="$(classify sfc "$f" "$tmp/golden" "${NORM[@]}")"; rc=$?
        if [ "$rc" -eq "$want" ]; then echo "  PASS: $desc -> $(echo "$out" | head -1 | cut -d: -f1)"; else
            echo "  FAIL: $desc: rc=$rc want=$want [$out]"; fails=$((fails+1)); fi
        ARTIFICE_TELL=""
    }

    printf '%s\n' '$ SHOW MEMORY' '  Main Memory   262144   217508' > "$tmp/match";        expect "MATCH (diff numbers, same shape)" 0 "$tmp/match"
    printf '%s\n' '$ SHOW MEMORY' '%DCL-W-IVVERB, unrecognized command' > "$tmp/missing";   expect "MISSING (IVVERB, no data)"          2 "$tmp/missing"
    printf '%s\n' '$ SHOW MEMORY'                                       > "$tmp/hollow";    expect "HOLLOW (echo only, no data body)"  3 "$tmp/hollow"
    printf '%s\n' '$ SHOW MEMORY' '  Main Memory   FAKE123  FAKE123'    > "$tmp/artifice";  expect "ARTIFICE-TELL (declared signature)" 4 "$tmp/artifice" 'FAKE[0-9]+'
    printf '%s\n' '$ SHOW MEMORY' '  Totally Wrong Layout   1   2   3'  > "$tmp/divergent"; expect "FORMAT-DIVERGENT (wrong shape)"      5 "$tmp/divergent"

    # negctl: the classifier is not vacuous -- a MATCH fixture must NOT classify
    # as MISSING/HOLLOW, and the MISSING fixture must NOT classify as MATCH.
    local m; m="$(classify sfc "$tmp/match" "$tmp/golden" "${NORM[@]}")"
    if echo "$m" | grep -q '^MATCH'; then echo "  PASS: NEGCTL match!=missing/hollow"; else echo "  FAIL: negctl match misclassified [$m]"; fails=$((fails+1)); fi

    echo "=== $( [ $fails -eq 0 ] && echo 'selftest OK' || echo "selftest FAILED ($fails)" ) ==="
    return $fails
}

# --- main ---
[ $# -ge 1 ] || { grep -E '^#( |$)' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
[ "$1" = selftest ] && { selftest; exit $?; }

SURFACE="$1"; shift
SURF_FILE="$SURF_DIR/$SURFACE.surface"
GOLDEN="$GOLDEN_DIR/$SURFACE.golden"
[ -f "$SURF_FILE" ] || die "no surface '$SURFACE' ($SURF_FILE)"
[ -f "$GOLDEN" ]    || die "no golden for '$SURFACE' ($GOLDEN) -- capture it first with capture_oracle.sh"

ARCH=""; DESC=""; NORMALIZE=""; ARTIFICE_TELL=""; COMMANDS=()
# shellcheck disable=SC1090
. "$SURF_FILE"

INPUT=""; GSELF=0
while [ $# -gt 0 ]; do
    case "$1" in
        --input) INPUT="$2"; shift 2 ;;
        --golden-self) GSELF=1; shift ;;
        *) die "unknown arg '$1'" ;;
    esac
done

TMP="$(mktemp)"; trap 'rm -f "$TMP"' EXIT
if [ "$GSELF" = 1 ]; then cp "$GOLDEN" "$TMP"        # sanity: golden already masked -> MATCH
elif [ -n "$INPUT" ]; then cp "$INPUT" "$TMP"
else cat > "$TMP"; fi

echo "== diff_surface: $SURFACE (arch=$ARCH) =="
classify "$SURFACE" "$TMP" "$GOLDEN" "$CAPTURE" normalize "$SURFACE"
exit $?
