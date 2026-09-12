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
# THE ONE DEFECT (vms-8e7)
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
# GROWN (vms-55e) to six more Bucket-D TUs, each the same shape: one minimal
# single-property mutation, isolated to ONE existing R1 suite, with its
# require_fail set MEASURED (not guessed) against a real host build --
#
#   codec-vc-zero-incarnation-not-refused    vms_cluster_codec_vc.c
#   codec-cm-short-body-not-refused          vms_cluster_codec_cm.c
#   codec-blk-no-trailer-not-honest          vms_cluster_codec_blk.c
#   barrier-bit0-uncounted                   vms_cnxman_barrier_fsm.c
#   phase2-count-mismatch-uncounted          vms_cnxman_phase2.c
#   recnx-last-gasp-uncounted                vms_cnxman_recnx_fsm.c
#   ldwv-refusal-uncounted                   vms_dlm_ldwv.c
#
# (vms_dlm_ldwv.c above -- FC-P4.3's Lock Directory Weight Vector -- was the
# seventh TU named by the item that grew this manifest; see its own defect
# entry below for the shape.)
#
SELF="$0"

DEFECTS="coord-genesis-refusal-uncounted
codec-vc-zero-incarnation-not-refused
codec-cm-short-body-not-refused
codec-blk-no-trailer-not-honest
barrier-bit0-uncounted
phase2-count-mismatch-uncounted
recnx-last-gasp-uncounted
ldwv-refusal-uncounted"

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

    codec-vc-zero-incarnation-not-refused)
        case "$_f" in
        facility)     echo "the §4(i).B incarnation-echo INV-6 gate in vms_scs_seq_stamp() (test_codec_vc.c's E66 case: a zero incarnation is never an honest value to stamp)";;
        targets)      echo "kernel-core/vms_cluster_codec_vc.c";;
        suites_red)   echo "test_codec_vc";;
        isolation)    echo "isolated";;
        why)          echo "vms_scs_seq_stamp()'s 'incarnation == 0 -> VMS_CODEC_E_INVAL' refusal is disabled, so a zero incarnation -- which appears in 0 of 239,981 reference frames -- is stamped onto the wire like any other value instead of being refused.";;
        require_fail) cat <<'EOF'
a zero incarnation is REFUSED, never written
and the refused frame's span was not touched at all
EOF
                      ;;
        esac;;

    codec-cm-short-body-not-refused)
        case "$_f" in
        facility)     echo "vms_cm_body_build()'s body_len length gate (the DLM-reply wrapper must never tail-pad a short caller buffer)";;
        targets)      echo "kernel-core/vms_cluster_codec_cm.c";;
        suites_red)   echo "test_codec_cm";;
        isolation)    echo "isolated";;
        why)          echo "vms_cm_body_build()'s 'body_len != VMS_CM_BODY_LEN -> VMS_CODEC_E_INVAL' refusal is disabled, so a body one byte short of the 132-byte SYSAP body is wrapped and sent anyway instead of being refused.";;
        require_fail) cat <<'EOF'
  a short body is rejected rather than tail-padded with whatever the caller's buffer held
EOF
                      ;;
        esac;;

    codec-blk-no-trailer-not-honest)
        case "$_f" in
        facility)     echo "vms_blk_trailer_parse()'s 'no trailer' honesty rule (design's TRAP 1: frame_len == inner_frame_len means nothing was piggybacked, not an error)";;
        targets)      echo "kernel-core/vms_cluster_codec_blk.c";;
        suites_red)   echo "test_pe_block";;
        isolation)    echo "isolated";;
        why)          echo "vms_blk_trailer_parse()'s early 'frame_len <= inner_frame_len -> VMS_CODEC_OK, no trailer' return is disabled, so the exact-length case that legitimately carries no piggyback falls through to the length arithmetic below and is refused VMS_CODEC_E_SHORT instead of being answered as the common, honest, no-trailer case. (A real READ end message always carries at least the 28-byte header, so this path is only exercised by TRAP 1's own synthetic case -- a receiver bounding the frame at the message's OWN declared length, which never sees the trailer at all.)";;
        require_fail) cat <<'EOF'
parsing with the DECLARED bound is not an error
EOF
                      ;;
        esac;;

    barrier-bit0-uncounted)
        case "$_f" in
        facility)     echo "the barrier's nodemap bit-0 instrumentation (book p. 7-25: CSV slot 0 is never used, so a set bit 0 is a width tell that must be COUNTED, never silently folded into the popcount)";;
        targets)      echo "kernel-core/vms_cnxman_barrier_fsm.c";;
        suites_red)   echo "test_cnxman_barrier";;
        isolation)    echo "isolated";;
        why)          echo "the barrier's 'b->bitmap_bit0++;' instrumentation, taken when an OPEN/ADD's nodemap byte has the impossible bit 0 set, is dropped. The bitmap is still read and processed identically -- only the counter that tells an operator the impossible bit fired goes silent.";;
        require_fail) cat <<'EOF'
the impossible bit is counted
EOF
                      ;;
        esac;;

    phase2-count-mismatch-uncounted)
        case "$_f" in
        facility)     echo "the p. 7-42 Phase 2 commit's count-mismatch accounting (phase2_commit_count(): a disagreement between the committed CSB count and the wire's own popcount is the tell for a nodemap byte too narrow to read)";;
        targets)      echo "kernel-core/vms_cnxman_phase2.c";;
        suites_red)   echo "test_cnxman_barrier";;
        isolation)    echo "isolated";;
        why)          echo "phase2_commit_count()'s 'st->count_mismatch++;' is dropped from the branch that fires when the committed member count differs from the transition's own nodemap popcount. The count itself (and the %CNXMAN log line) is still computed and logged correctly -- only the counter an operator or a later test reads back goes silent.";;
        require_fail) cat <<'EOF'
and the disagreement with the nodemap is COUNTED -- which is exactly the width tell
counted
EOF
                      ;;
        esac;;

    recnx-last-gasp-uncounted)
        case "$_f" in
        facility)     echo "the p. 7-29/7-49 last-gasp accounting (cnxman_recnx_shutdown(): the SHUTDOWN datagram this node emits to the peers it is leaving must be counted, not just sent)";;
        targets)      echo "kernel-core/vms_cnxman_recnx_fsm.c";;
        suites_red)   echo "test_cnxman_recnx";;
        isolation)    echo "isolated";;
        why)          echo "cnxman_recnx_shutdown()'s 'r->last_gasps++;' is dropped. The CLUB/CSB SHUTDOWN flags are still set and the last-gasp record is still emitted to the caller -- only the counter that tells an operator one was sent goes silent.";;
        require_fail) cat <<'EOF'
counted once
EOF
                      ;;
        esac;;

    ldwv-refusal-uncounted)
        case "$_f" in
        facility)     echo "the p. 6-33 Lock Directory Weight Vector's refusal accounting (cnxman_ldwv_rebuild(): a mixed-kind or foreign-member refusal must be counted, the rd vms-1ee split-brain teeth)";;
        targets)      echo "kernel-core/vms_dlm_ldwv.c";;
        suites_red)   echo "test_dlm_ldwv";;
        isolation)    echo "isolated";;
        why)          echo "cnxman_ldwv_rebuild()'s 'club->ldwv_build_refused++;' in the survey-verdict refusal branch (VMS_LDWV_E_WEIGHTS/VMS_LDWV_E_FOREIGN) is dropped -- range-anchored to that FIRST refusal site only, leaving the second (ldwv_fill_club's own VMS_LDWV_E_TOOBIG path) untouched. The refusal itself (no vector left behind, the %CNXMAN log line) still fires -- only the count goes silent.";;
        require_fail) cat <<'EOF'
and counted in the CLUB
the refusal is counted
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

    codec-vc-zero-incarnation-not-refused)
        # `if (incarnation == 0u)` is unique in this file (the credit-return
        # gate a few functions down reads `c->incarnation`, a different
        # literal) -- disarmed, not deleted, so the parameter stays used.
        sed -i 's|if (incarnation == 0u)|if (0 \&\& incarnation == 0u) /* NEGCTL codec-vc-zero-incarnation-not-refused */|' "$_file";;

    codec-cm-short-body-not-refused)
        # `if (body_len != VMS_CM_BODY_LEN)` is unique in this file.
        sed -i 's|if (body_len != VMS_CM_BODY_LEN)|if (0 \&\& body_len != VMS_CM_BODY_LEN) /* NEGCTL codec-cm-short-body-not-refused */|' "$_file";;

    codec-blk-no-trailer-not-honest)
        # `if (frame_len <= inner_frame_len)` is unique in this file.
        sed -i 's|if (frame_len <= inner_frame_len)|if (0 \&\& frame_len <= inner_frame_len) /* NEGCTL codec-blk-no-trailer-not-honest */|' "$_file";;

    barrier-bit0-uncounted)
        # `b->bitmap_bit0++;` is unique in this file, and it is the
        # braceless body of an `if (...)`. An empty COMPOUND statement
        # (`{ }`), not a bare `;` -- this library builds -Wall -Wextra
        # -Werror, and a lone `;` after `if` trips -Werror=empty-body. `{ }`
        # is a legal, warning-free empty statement.
        sed -i 's|b->bitmap_bit0++;|{ } /* NEGCTL barrier-bit0-uncounted: the impossible bit is not counted */|' "$_file";;

    phase2-count-mismatch-uncounted)
        # `st->count_mismatch++;` occurs ONCE in this file (the other
        # counters phase2_commit_count() bumps -- bitmap_short,
        # m_above_grounded -- have their own, differently-named lines), so no
        # range anchor is needed: grep -c is 1.
        sed -i 's|st->count_mismatch++;|/* NEGCTL phase2-count-mismatch-uncounted: the disagreement is not counted */|' "$_file";;

    recnx-last-gasp-uncounted)
        # `r->last_gasps++;` is unique in this file.
        sed -i 's|r->last_gasps++;|/* NEGCTL recnx-last-gasp-uncounted: the last gasp is not counted */|' "$_file";;

    ldwv-refusal-uncounted)
        # `club->ldwv_build_refused++;` occurs TWICE in this file (the
        # survey-verdict refusal this defect targets, and ldwv_fill_club's
        # own VMS_LDWV_E_TOOBIG refusal a few lines later) -- IDENTICAL text,
        # so a plain sed would mutate both and trip two properties at once.
        # Range-anchored (facility_defects.sh's devtab-owner-not-recorded
        # precedent), NOT `0,/re/` first-match: the range starts at
        # `ldwv_survey_club(club, &s);` (unique, immediately precedes the
        # verdict call) and ends at the FIRST `return st;` after it, which is
        # the closing statement of the block this defect targets and stops
        # short of the second occurrence entirely. Also idempotency-safe: a
        # second apply finds the range's own `club->ldwv_build_refused++;`
        # already gone, so cmd_apply's pristine-compare reports BROKEN
        # FIXTURE rather than silently moving on to the second occurrence.
        sed -i '/^\tldwv_survey_club(club, &s);$/,/^\t\treturn st;$/ s|club->ldwv_build_refused++;|/* NEGCTL ldwv-refusal-uncounted: the refusal is not counted */|' "$_file";;

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
