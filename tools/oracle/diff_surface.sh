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

# apply_may_omit -- stdin -> stdout, removing each declared substrate-ABSENT
# SECTION: a header line matching a MAY_OMIT pattern plus its data rows up to (and
# including) the next blank line. Applied to BOTH the golden AND OVMX before the
# MATCH compare, so a GROUNDED omission (the substrate genuinely lacks the
# facility -- vms-8019 honest-omission) does not red the diff. An UNDECLARED
# omission leaves the golden's section in place and reds (HOLLOW/FORMAT-DIVERGENT).
# ⚠ MAY_OMIT is for substrate-ABSENT facilities ONLY -- a field OVMX could source
# but doesn't render is a real HOLLOW gap and must NEVER be listed here; that is
# the allowlist-to-pass INV-6 forbids. Each surface's MAY_OMIT is grounded in its
# .surface file against the battery's existing must_not_have.
apply_may_omit() {
    if [ -z "${MAY_OMIT:-}" ]; then cat; return; fi
    awk -v pats="$MAY_OMIT" '
        BEGIN { n = split(pats, a, "|") }
        skip  { if ($0 ~ /^[[:space:]]*$/) skip = 0; next }
        { for (i = 1; i <= n; i++) if (a[i] != "" && index($0, a[i])) { skip = 1; next }
          print }'
}

# strip_console -- stdin -> stdout, dropping the command-ECHO lines (a COMMANDS
# entry, with or without a leading "$ " prompt) and bare "$ " prompt lines. The
# oracle capture and the battery's run_cmd echo/prompt DIFFERENTLY: the golden is
# `$ SHOW MEMORY\n<output>` (capture_oracle's "$ CMD" echo, no trailing prompt)
# while run_cmd's $SEG is `SHOW MEMORY\n<output>\n$ ` (bare echo + returned
# prompt). Stripping both to the pure OUTPUT BODY makes the gate compare LAYOUT,
# not console framing. COMMANDS is global (from the sourced surface); with none
# set it strips only bare prompts (leaving fixture echoes for the selftest).
strip_console() {
    if [ "${#COMMANDS[@]}" -eq 0 ]; then grep -vE '^\$[[:space:]]*$' || true; return; fi
    grep -vxF -f <(for c in "${COMMANDS[@]}"; do printf '%s\n$ %s\n' "$c" "$c"; done) \
      | grep -vE '^\$[[:space:]]*$' || true
}

# structure_norm -- stdin -> stdout. The CROSS-SYSTEM structure-tolerant transform
# (vms-c38, conductor ruling 2026-08-30). A byte-exact column-geometry gate is
# IMPOSSIBLE cross-system: OVMX's VALUES legitimately differ from the VAX/Alpha
# oracle's -- wider numbers (OVMX has more RAM/pages than a VAXserver 3900) and
# different machine strings (CPU model, node name, device names). So the gate
# proves STRUCTURAL fidelity: same sections, labels, headers, and FIELD STRUCTURE,
# tolerant of legitimate value differences -- NOT of a missing or HOLLOW field.
# Three transforms, applied to BOTH golden and OVMX (symmetric, like NORMALIZE):
#   1. MACHINE_MASK (per-surface, grounded): sed s/// program masking fields that
#      legitimately vary by machine (CPU model, node, device name). Each s///
#      replaces a NON-EMPTY match with a fixed token, so a blank/absent field does
#      NOT match and STILL REDS -- a machine-mask means "this value varies, don't
#      compare it," NEVER "ignore this field" (INV-6, same discipline as MAY_OMIT).
#   2. collapse a digit-mask RUN to ONE token ('#+' -> '#'): any-width masked number
#      -> the same token, so a 6-digit VAX count and an 8-digit OVMX count compare
#      equal. ⚠ A BLANK stays blank (no '#'), so a HOLLOW numeric field STILL REDS.
#   3. whitespace-normalize (runs -> one space, trim): shifted column positions
#      (from differing value widths) compare equal by structure.
# Applied LAST, after apply_may_omit/strip_console, so those still match literal
# section headers / command echoes on the un-collapsed text.
structure_norm() {
    local mm="${MACHINE_MASK:-}"
    { [ -n "$mm" ] && sed -E "$mm" || cat; } \
      | sed -E 's/#+/#/g' \
      | sed -E 's/[[:space:]]+/ /g; s/^[[:space:]]+//; s/[[:space:]]+$//'
}

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

    # 4/5. MATCH vs FORMAT-DIVERGENT -- normalize both sides symmetrically, then
    # strip declared substrate-absent sections (MAY_OMIT) from BOTH before compare.
    ovmx_norm="$("$@" < "$ovmx")"
    local ovmx_cmp golden_cmp
    ovmx_cmp="$(printf '%s\n' "$ovmx_norm" | apply_may_omit | strip_console | structure_norm)"
    golden_cmp="$(apply_may_omit < "$golden" | strip_console | structure_norm)"
    if [ "$ovmx_cmp" = "$golden_cmp" ]; then
        echo "MATCH: '$surface' is VMS-faithful${MAY_OMIT:+ (modulo grounded substrate-omissions: $MAY_OMIT)}."
        return 0
    fi

    echo "FORMAT-DIVERGENT: '$surface' has data but the wrong shape (normalized diff after grounded MAY_OMIT, golden < vs OVMX >):"
    diff <(printf '%s\n' "$golden_cmp") <(printf '%s\n' "$ovmx_cmp") | sed 's/^/    /' | head -40
    return 5
}

selftest() {
    COMMANDS=(); MACHINE_MASK=""   # strip_console/structure_norm read them; unset trips set -u before main inits
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

    # MAY_OMIT: a 3-section golden (single-digit fields keep the mask widths
    # equal). A DECLARED substrate-absent section (Virtual I/O Cache) omitted by
    # OVMX -> MATCH; an UNDECLARED omission (Dynamic) -> reds.
    printf '%s\n' '$ SHOW MEMORY' '  Physical   #   #' '' '  Virtual I/O Cache   #   #' '' '  Dynamic   #   #' > "$tmp/g3"
    printf '%s\n' '$ SHOW MEMORY' '  Physical   1   2' '' '  Dynamic   7   8' > "$tmp/omit_ok"
    printf '%s\n' '$ SHOW MEMORY' '  Physical   1   2' > "$tmp/omit_bad"
    local o rc
    MAY_OMIT='Virtual I/O Cache'
    o="$(classify sfc "$tmp/omit_ok" "$tmp/g3" "${NORM[@]}")"; rc=$?
    if [ "$rc" -eq 0 ]; then echo "  PASS: MAY_OMIT tolerates the DECLARED substrate-absent section -> MATCH"; else
        echo "  FAIL: MAY_OMIT declared-omit not MATCH: rc=$rc [$o]"; fails=$((fails+1)); fi
    o="$(classify sfc "$tmp/omit_bad" "$tmp/g3" "${NORM[@]}")"; rc=$?
    if [ "$rc" -ne 0 ]; then echo "  PASS: MAY_OMIT does NOT tolerate an UNDECLARED omission (Dynamic dropped -> red rc=$rc)"; else
        echo "  FAIL: undeclared omission passed as MATCH [$o]"; fails=$((fails+1)); fi
    MAY_OMIT=''

    # structure_norm (vms-c38): the CROSS-SYSTEM value-tolerant compare.
    # (i) a digit-mask WIDTH difference (VAX 6-digit vs OVMX 8-digit) now MATCHes
    #     -- the whole reason a byte-exact digit-mask can't gate cross-system.
    printf '%s\n' '$ SHOW MEMORY' '  Main Memory   ####   ####' > "$tmp/gw"
    printf '%s\n' '$ SHOW MEMORY' '  Main Memory   262144   99'  > "$tmp/wwide"
    o="$(classify sfc "$tmp/wwide" "$tmp/gw" "${NORM[@]}")"; rc=$?
    if [ "$rc" -eq 0 ]; then echo "  PASS: structure_norm tolerates a digit-WIDTH difference (6- vs 2-digit -> MATCH)"; else
        echo "  FAIL: width-difference not MATCH: rc=$rc [$o]"; fails=$((fails+1)); fi
    # (ii) GUARDRAIL 1: a BLANK where the golden has a number must STILL RED (a
    #      hollow numeric field is not a value difference -- collapse keeps it red).
    printf '%s\n' '$ SHOW MEMORY' '  Main Memory' > "$tmp/wblank"
    o="$(classify sfc "$tmp/wblank" "$tmp/gw" "${NORM[@]}")"; rc=$?
    if [ "$rc" -ne 0 ]; then echo "  PASS: structure_norm keeps a HOLLOW numeric field RED (blank != number-token, rc=$rc)"; else
        echo "  FAIL: hollow-numeric passed as MATCH -- guardrail 1 broken [$o]"; fails=$((fails+1)); fi
    # (iii) MACHINE_MASK: a grounded per-field mask lets a legitimately-varying
    #       machine string (node name) MATCH, but GUARDRAIL 3 -- a blank/absent
    #       masked field must STILL RED (mask means "value varies," not "ignore").
    printf '%s\n' '$ SHOW CPU' '  Node: VAX#' > "$tmp/gm"
    printf '%s\n' '$ SHOW CPU' '  Node: OVMX' > "$tmp/mreal"
    printf '%s\n' '$ SHOW CPU' '  Node:'      > "$tmp/mblank"
    # ⚠ masks run on DIGIT-MASKED text -- a field with digits reads as '#', so the
    #   char class must include '#' (node 'VAX#' = 'VAX' + a masked digit).
    MACHINE_MASK='s/Node: [A-Z0-9#]+/Node: <NODE>/'
    o="$(classify sfc "$tmp/mreal" "$tmp/gm" "${NORM[@]}")"; rc=$?
    if [ "$rc" -eq 0 ]; then echo "  PASS: MACHINE_MASK matches a present+non-empty varying field (Node OVMX vs VAX# -> MATCH)"; else
        echo "  FAIL: machine-mask real-field not MATCH: rc=$rc [$o]"; fails=$((fails+1)); fi
    o="$(classify sfc "$tmp/mblank" "$tmp/gm" "${NORM[@]}")"; rc=$?
    if [ "$rc" -ne 0 ]; then echo "  PASS: MACHINE_MASK keeps a BLANK masked field RED (guardrail 3: mask != ignore, rc=$rc)"; else
        echo "  FAIL: blank masked field passed as MATCH -- guardrail 3 broken [$o]"; fails=$((fails+1)); fi
    # (iv) GUARDRAIL 3, a '.+'-terminated value mask (the SHOW CPU MP-STATE style,
    #      'Label: <free text>'): must require NON-EMPTY too. A '.*' terminator would
    #      match an empty value and let a hollow 'Multiprocessing is ' PASS -- this
    #      case reds a blank ONLY if the mask uses '.+' (VAX-lane review catch, so a
    #      future regression back to '.*' fails here).
    printf '%s\n' '$ SHOW CPU' 'Multiprocessing is #'       > "$tmp/gmp"
    printf '%s\n' '$ SHOW CPU' 'Multiprocessing is ENABLED.' > "$tmp/mp_real"
    printf '%s\n' '$ SHOW CPU' 'Multiprocessing is '        > "$tmp/mp_blank"
    MACHINE_MASK='s/^(Multiprocessing is ).+/\1<MP-STATE>/'
    o="$(classify sfc "$tmp/mp_real" "$tmp/gmp" "${NORM[@]}")"; rc=$?
    if [ "$rc" -eq 0 ]; then echo "  PASS: '.+' value mask matches a present+non-empty MP-state (ENABLED vs # -> MATCH)"; else
        echo "  FAIL: .+ mask real-field not MATCH: rc=$rc [$o]"; fails=$((fails+1)); fi
    o="$(classify sfc "$tmp/mp_blank" "$tmp/gmp" "${NORM[@]}")"; rc=$?
    if [ "$rc" -ne 0 ]; then echo "  PASS: '.+' value mask keeps a BLANK MP-state RED (rejects '.*'-matches-empty hollow, rc=$rc)"; else
        echo "  FAIL: blank MP-state passed as MATCH -- '.+' guardrail broken (mask uses '.*'?) [$o]"; fails=$((fails+1)); fi
    MACHINE_MASK=''

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

ARCH=""; DESC=""; NORMALIZE=""; ARTIFICE_TELL=""; MAY_OMIT=""; MACHINE_MASK=""; COMMANDS=()
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
