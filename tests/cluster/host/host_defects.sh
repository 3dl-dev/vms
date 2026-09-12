#!/bin/sh
#
# host_defects.sh - the R1 host-side mutation manifest for
# tests/cluster/host (vms-8e7).
#
# WHY THIS EXISTS
#
# tests/qemu/facility_defects.sh proves the QEMU/real-/dev/vms executive
# facilities can go red under an injected defect. The 46 R1 host suites in
# THIS directory (tests/cluster/host/) had NO analogous gate: nothing proved
# a single one of them could ever go red. A suite that has never been shown
# capable of failing is not a control, it is a tautology with printf calls.
#
# This file is the FIRST INCREMENT of that gate: one minimal defect, so the
# pattern exists and every later Bucket D/E TU can be added the same way --
# one DEFECTS entry, one `defect_field` case, one `apply_edit` case, one
# `/* negctl: <name> */` anchor above the assertion(s) it names.
#
# SHAPE, DELIBERATELY BORROWED FROM tests/qemu/facility_defects.sh
#
# Same idiom (DEFECTS list, a per-defect `case` exposing facility/targets/
# suites_red/isolation/why/require_fail, an `apply` subcommand that sed(1)s
# the source and PROVES the edit landed by comparing against a pristine copy,
# a `list` subcommand, and a `selftest` that applies to a throwaway copy and
# asserts the anchor still matches) -- but HOST-NATIVE: no QEMU, no /dev/vms,
# no vms.ko. tests/cluster/host builds with a plain host C compiler against
# src/kernel-core alone (see tests/cluster/host/CMakeLists.txt's own header),
# so the driver that actually EXECUTES a mutation
# (tests/cluster/host/run_host_negctl.sh) is a cmake build + run, not a QEMU
# boot. THIS FILE, like facility_defects.sh, is otherwise STATIC: `list`,
# `field` and the injection half of `apply` touch no suite and prove nothing
# about what goes red. Only run_host_negctl.sh executes anything.
#
# NOT PORTED (deliberately, this increment): the coverage/floor/scope-out
# machinery (facility_defects.sh's cmd_coverage, the count floor, the
# SCOPE_OUT_* declarations). With one defect there is nothing to floor or
# scope; that machinery belongs to the item that grows DEFECTS to cover the
# other 17 Bucket D/E TUs.
#
# THE ONE DEFECT
#
#   coord-genesis-refusal-uncounted   src/kernel-core/vms_cnxman_coord_fsm.c
#   drops the `c->genesis_refused_noquorum++;` counter bump inside the
#   NO_QUORUM refusal branch of cnxman_coord_found()'s genesis path. The
#   refusal itself (coord_found_refuse(..., CNXMAN_COORD_REF_NO_QUORUM, ...))
#   is UNTOUCHED -- a node that may not found still refuses to found -- only
#   the anti-LARP tripwire that counts how many times it refused goes silent.
#   That is exactly the shape of defect this suite's own header warns about:
#   "a thousand refusals must still be a thousand refusals -- nothing
#   accumulates toward a mint", proven only by a counter nobody would notice
#   is wrong from the refusal's rc/state/CLUB-unchanged side alone.
#
SELF="$0"

DEFECTS="coord-genesis-refusal-uncounted"

# ---------------------------------------------------------------------------
# Metadata (same field meanings as tests/qemu/facility_defects.sh):
#   facility     human-readable name of the property under test.
#   targets      source files, relative to a src/ root, the mutation edits.
#   suites_red   the suite(s) this defect is allowed to redden.
#   isolation    isolated - every suite must produce a verdict, no suite
#                           outside suites_red may fail.
#   why          the single sentence describing the mutation and its effect.
#   require_fail assertion texts (the ct_check/ct_check_eq_u32 `what` label,
#                with any ": got ..." tail stripped) that MUST appear in the
#                observed red set, and the ONLY texts that may -- run_host_
#                negctl.sh asserts the two sets are EQUAL.
# ---------------------------------------------------------------------------
defect_field() {
    _d="$1"; _f="$2"
    case "$_d" in

    coord-genesis-refusal-uncounted)
        case "$_f" in
        facility)     echo "cluster GENESIS founding-refusal accounting (cnxman_coord_found(), the anti-LARP tripwire docs/design-cluster-genesis.md warns must never drift)";;
        targets)      echo "kernel-core/vms_cnxman_coord_fsm.c";;
        suites_red)   echo "test_cnxman_genesis_negctl";;
        isolation)    echo "isolated";;
        why)          echo "cnxman_coord_found()'s NO_QUORUM branch stops incrementing genesis_refused_noquorum. The refusal itself (rc, last_refusal, state, and the byte-for-byte CLUB) is untouched -- only the count of how many times the node refused to found goes silent, so a node that should show N counted refusals shows one fewer than it really performed.";;
        require_fail) cat <<'EOF'
the refusal is COUNTED (the anti-LARP tripwire)
counted
every attempt was refused, and counted
EOF
                      ;;
        esac;;

    *)
        echo "host_defects.sh: unknown defect '$_d'" >&2
        return 1;;
    esac
}

# ---------------------------------------------------------------------------
# apply_edit <file> <defect>
#
# The mutation itself. One sed per defect, anchored to an exact source line.
# The replacement text carries a `/* NEGCTL <name> */` marker so a second
# apply against the same file finds no match -- cmd_apply's pristine-compare
# then reports BROKEN FIXTURE instead of silently re-applying nothing.
# ---------------------------------------------------------------------------
apply_edit() {
    _file="$1"; _d="$2"
    case "$_d" in
    coord-genesis-refusal-uncounted)
        # c->genesis_refused_noquorum++; is unique in this file (grep -c is
        # 1) -- no line anchor needed. The refusal three lines below is left
        # completely alone.
        sed -i 's|c->genesis_refused_noquorum++;|/* NEGCTL coord-genesis-refusal-uncounted: the refusal is not counted */|' "$_file";;
    *)
        echo "host_defects.sh: unknown defect '$_d'" >&2
        return 1;;
    esac
}

cmd_list() { echo "$DEFECTS"; }

cmd_field() {
    [ $# -eq 2 ] || { echo "usage: host_defects.sh field <defect> <field>" >&2; return 2; }
    defect_field "$1" "$2"
}

# ---------------------------------------------------------------------------
# cmd_apply <defect> <src-root>...
#
# Applies the defect to every copy of every target file that exists under the
# given src roots, and PROVES the edit landed by comparing each file against a
# pristine copy taken immediately before the edit. An anchor that no longer
# matches is a BROKEN FIXTURE: the build must fail loudly rather than produce
# an unmutated binary that then "proves" the gate caught nothing.
# (Same shape as tests/qemu/facility_defects.sh's cmd_apply.)
# ---------------------------------------------------------------------------
cmd_apply() {
    [ $# -ge 2 ] || { echo "usage: host_defects.sh apply <defect> <src-root>..." >&2; return 2; }
    _d="$1"; shift

    defect_field "$_d" targets >/dev/null || return 2
    _targets=$(defect_field "$_d" targets)

    command -v cmp >/dev/null 2>&1 || {
        echo "FATAL: cmp(1) unavailable -- cannot verify the injection landed" >&2
        return 3
    }

    _touched=0
    for _root in "$@"; do
        [ -d "$_root" ] || continue
        for _t in $_targets; do
            _f="$_root/$_t"
            [ -f "$_f" ] || continue
            cp "$_f" "$_f.negctl-pristine" || return 3
            apply_edit "$_f" "$_d" || return 3
            if cmp -s "$_f" "$_f.negctl-pristine"; then
                echo "FATAL: BROKEN FIXTURE (not a broken gate)." >&2
                echo "  defect '$_d' did not change $_f -- its sed anchor no longer matches." >&2
                echo "  The source moved; re-anchor the mutation in tests/cluster/host/host_defects.sh." >&2
                rm -f "$_f.negctl-pristine"
                return 3
            fi
            rm -f "$_f.negctl-pristine"
            echo "  injected '$_d' into $_f"
            _touched=$((_touched + 1))
        done
    done

    if [ "$_touched" -eq 0 ]; then
        echo "FATAL: BROKEN FIXTURE (not a broken gate)." >&2
        echo "  defect '$_d' names target(s) [$_targets] but none exist under: $*" >&2
        return 3
    fi
    return 0
}

# ---------------------------------------------------------------------------
# cmd_selftest <repo-root>
#
# STATIC, like facility_defects.sh's own selftest: proves the sed anchor
# still matches the current tree (a real sed(1) run against a throwaway
# copy, then diffed) and that re-applying to the same copy is refused as a
# BROKEN FIXTURE rather than silently landing nothing. Does NOT compile or
# run anything -- that claim belongs to run_host_negctl.sh alone.
# ---------------------------------------------------------------------------
cmd_selftest() {
    [ $# -eq 1 ] || { echo "usage: host_defects.sh selftest <repo-root>" >&2; return 2; }
    _st_repo="$1"
    _st_root="$_st_repo/src"
    _st_tests="$_st_repo/tests/cluster/host"
    _st_tmp=$(mktemp -d) || return 2
    _st_rc=0

    for _st_d in $DEFECTS; do
        for _st_fld in facility targets suites_red isolation why require_fail; do
            if [ -z "$(defect_field "$_st_d" "$_st_fld")" ]; then
                echo "FAIL: $_st_d: metadata field '$_st_fld' is empty"
                _st_rc=1
            fi
        done

        rm -rf "$_st_tmp/tree"
        mkdir -p "$_st_tmp/tree"
        if ! cp -a "$_st_root/kernel-core" "$_st_tmp/tree/" 2>/dev/null; then
            echo "FAIL: cannot copy $_st_root/kernel-core for the self-test"
            rm -rf "$_st_tmp"
            return 2
        fi

        if cmd_apply "$_st_d" "$_st_tmp/tree" >/dev/null 2>&1; then
            echo "  ok: $_st_d injects into the current tree"
        else
            echo "FAIL: $_st_d: its sed anchor no longer matches the source tree."
            echo "      The mutation would inject NOTHING and run_host_negctl.sh would then"
            echo "      certify a defect that was never applied. Re-anchor it."
            _st_rc=1
            continue
        fi

        if _st_out=$(cmd_apply "$_st_d" "$_st_tmp/tree" 2>&1); then
            echo "FAIL: $_st_d: applying it TWICE succeeded. Either the mutation is repeatable"
            echo "      (so it is not the single minimal edit it claims to be) or the"
            echo "      injection-landed check never fires -- in which case a dead anchor"
            echo "      would be reported as a caught defect."
            _st_rc=1
        elif echo "$_st_out" | grep -qF 'BROKEN FIXTURE'; then
            echo "  ok: $_st_d: a no-op re-apply is reported as BROKEN FIXTURE (the check has teeth)"
        else
            echo "FAIL: $_st_d: re-apply failed, but not with BROKEN FIXTURE: $_st_out"
            _st_rc=1
        fi
    done

    rm -rf "$_st_tmp"

    # Every require_fail text has to EXIST literally in the suite source, or
    # the driver can never observe it and the manifest entry is a typo, not a
    # property. Loose normalisation (quotes/backslashes stripped, whitespace
    # collapsed) for the same reason facility_defects.sh's selftest does it:
    # this catches a typo, not a C parse.
    for _st_d in $DEFECTS; do
        for _st_suite_glob in $(defect_field "$_st_d" suites_red); do
            _st_src="$_st_tests/$_st_suite_glob.c"
            [ -f "$_st_src" ] || { echo "FAIL: $_st_d: suites_red names '$_st_suite_glob' but $_st_src does not exist"; _st_rc=1; continue; }
            _st_flat=$(tr -d '"\\' <"$_st_src" | tr '\n\t' '  ' | tr -s ' ')
            defect_field "$_st_d" require_fail | while IFS= read -r _st_txt; do
                [ -n "$_st_txt" ] || continue
                _st_needle=$(printf '%s' "$_st_txt" | tr -d '"\\' | tr -s ' ')
                case "$_st_flat" in
                *"$_st_needle"*) : ;;
                *) echo "FAIL: $_st_d: require_fail text absent from $_st_src: [$_st_txt]";;
                esac
            done
        done
    done

    if [ "$_st_rc" -eq 0 ]; then
        echo "PASS: every defect's sed mutation injects into the current tree (executed,"
        echo "      real sed + cmp against a throwaway copy), its injection-landed check"
        echo "      demonstrably fires on a no-op re-apply, and every require_fail text"
        echo "      appears literally in its suite's source (a text search, not a run --"
        echo "      only run_host_negctl.sh actually builds and runs anything)."
    fi
    return $_st_rc
}

case "${1:-}" in
    list)     shift; cmd_list "$@";;
    field)    shift; cmd_field "$@";;
    apply)    shift; cmd_apply "$@";;
    selftest) shift; cmd_selftest "$@";;
    *)  echo "usage: host_defects.sh {list|field|apply|selftest} ..." >&2; exit 2;;
esac
