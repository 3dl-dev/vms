#!/bin/sh
#
# facility_negctl_record.sh - the EXECUTION-SOURCED attribution record:
# writer, reader, validator and comparator (rd vms-d894, rd vms-659).
#
# WHY THIS FILE EXISTS
#
# tests/qemu/run_facility_negctl.sh is the ONE instrument in this program that
# executes anything: it injects each of the manifest's defects one at a time,
# rebuilds vms.ko and the suites in-container, boots QEMU against a real
# /dev/vms, and asserts the observed red set is EXACTLY what the manifest
# names -- behind a pristine positive control, so a harness that fails
# indiscriminately cannot pass it. Its results then died with the job log.
#
# Two static claims were left standing on nothing as a result:
#
#   vms-659   `facility_defects.sh coverage` said a suite was "PROVEN able to
#             go red" when what it had done was match the manifest's own
#             suites_red glob against the suite's own name. PR #65 made that
#             honest by saying "NAMED". The better answer -- derive the proven
#             set from what the driver actually observed -- needed a record.
#
#   vms-d894  nothing floored the SIZE of the DEFECTS list. PR #65 added a
#             count floor read from tests/qemu/facility_defects_floor.txt and
#             disclosed that it does not close the item: the floor is a
#             hand-set integer the deleter can edit in the same commit, so the
#             price of a deletion moved from 20 lines/2 files silently to ~21
#             lines/3 files visibly. The item asked for a floor sourced from
#             something the deleter does not also own.
#
# Both wanted the same missing ingredient, and it is the same one: a record of
# what a run OBSERVED, rather than a declaration the tree makes about itself.
#
# ===========================================================================
# WHAT A COMMITTED RECORD PROVES ON A HOST THAT DID NOT RUN THE DRIVER
# ===========================================================================
#
# In one sentence, and it is the sentence every consumer of this file must
# print rather than paraphrase:
#
#   A PAST RUN OF THE DRIVER, ON THE TREE NAMED IN THIS RECORD'S HEADER,
#   OBSERVED THESE RESULTS -- IT DOES NOT SAY THEY HOLD NOW.
#
# BUYS
#   - The count of defects a run actually EXECUTED, which the deleter does not
#     own in the ordinary sense: deleting a manifest entry today cannot
#     retroactively change what a past CI run observed, and shrinking the
#     record is a diff of visible per-assertion rows, not one integer.
#   - The set of suites in which a run actually saw an assertion FAIL. That is
#     the only population any "proven able to go red" claim may cover; every
#     other in-scope suite stays "NAMED"/"declared".
#   - INTEGRITY UNDER CI. run_facility_negctl.sh re-emits this record on every
#     CI run and compares it, row for row, with the committed copy. A
#     committed record therefore cannot be fabricated UPWARD and cannot go
#     stale silently: any disagreement between the committed rows and the live
#     observation is red. THAT comparison is the part the deleter does not own.
#   - CONTRADICTION IS A REFUSAL, not a note. A record naming a defect the
#     manifest no longer has, or an assertion text the manifest no longer
#     names, or produced by a run whose pristine positive control did not
#     pass, does not degrade the gate quietly -- the consumer refuses to
#     certify anything from it and says so.
#
# DOES NOT BUY
#   - It is a SNAPSHOT, exactly like tracking/rd-citations.tsv. It is a file in
#     the repo. A row written by hand reads exactly like an observed one to
#     this library. What catches that is not this file; it is the live CI
#     comparison above, and only there.
#   - It does not prove any assertion fires TODAY. Nothing on a host without a
#     real /dev/vms can prove that, and this file must never be read as if it
#     did.
#   - IT IS NOT TAMPER-PROOF, and this is the residual that stays open. A
#     deleter who removes a defect from the manifest AND removes that defect's
#     rows from this record has told the truth about a smaller manifest: the
#     live CI run agrees with the shrunken record, and nothing here disagrees.
#     Closing that needs a floor from OUTSIDE the commit -- the previous
#     commit's copy of this record, or an external attestation. This file does
#     not have one and does not pretend to. What it does raise is the price:
#     the rows are per-observed-assertion, so the deletion is proportional to
#     what is being deleted instead of being one integer.
#   - It says nothing about defects the manifest has gained since the run. Those
#     are counted and printed as UNPROVEN. Growth is incompleteness, not
#     contradiction, and refusing on it would red a correct edit -- which is
#     how a gate gets weakened by the next person.
#
# ===========================================================================
# THE FORMAT, AND WHY TSV
# ===========================================================================
#
#   # <prose header, then one "# <field>: <value>" line per header field>
#   RUN<TAB><defect><TAB><suites that went red, space-joined, or "-"><TAB><pass|fail>
#   RED<TAB><defect><TAB><suite or (harness)><TAB><assertion text observed FAILing>
#
# TSV, in keeping with tracking/rd-citations.tsv, and for four reasons that
# are about this record specifically:
#
#   1. Every consumer here is grep/awk/cut/comm. A format needing a parser
#      would put a parser between an execution result and the gate reading it,
#      and a parser is a place for a gate to fail open.
#   2. ONE OBSERVED ASSERTION PER LINE. The whole point is that shrinking the
#      record is expensive and visible; a per-defect row holding a packed list
#      would collapse an eight-assertion deletion into one line.
#   3. The manifest contains no tab anywhere (checked: `grep -c '\t'` on
#      facility_defects.sh is 0), so an assertion text can never split a field.
#      A row this reader cannot parse into exactly four fields is a REFUSAL,
#      not a skipped line -- skipping is how a record becomes an allowlist with
#      a typo in it.
#   4. It diffs line-for-line in review.
#
# NO CARDINAL IS STORED THAT CAN BE DERIVED. The number of defects executed is
# the number of RUN rows; the observed red suites are the distinct RED suites.
# Storing either would create a second, editable copy of a fact the rows
# already carry. Only the header fields below cannot be derived from the rows:
#
#   generated-at         YYYY-MM-DDTHH:MM:SSZ, when the run emitted this.
#   tree-commit          the commit the driver ran against, or "unknown" when
#                        the emitting host had no git answer. This is the
#                        "which tree" identity the record must carry.
#   positive-control     pass|fail -- the pristine run's result. A record whose
#                        positive control did not pass is a REFUSAL: every
#                        negative control under a failing positive control is
#                        unfounded, which is exactly why the driver runs it
#                        first.
#   defects-in-manifest  how many entries DEFECTS had when the run started, so
#                        a partial run (the driver takes an optional defect
#                        list) is legible as partial instead of reading as a
#                        shrunken manifest.
#   suites-derived       how many suite sources the driver derived from the
#                        checkout for that run.
#
# THERE IS DELIBERATELY NO MANIFEST DIGEST. A digest answers "did any byte of
# the manifest change" -- coarser than what this reader already does, and it
# would refuse on edits that are not contradictions (a reworded comment). The
# reader instead compares the record's defect NAMES and its observed assertion
# TEXTS against the live manifest directly, which distinguishes contradiction
# (refuse) from growth (count as unproven). See fnr_validate.
#
# USAGE (sourceable; also runnable for the two whole-file operations)
#
#   . "$(dirname "$0")/facility_negctl_record.sh"
#
#   fnr_record_path <tests-qemu-dir>            -> the conventional path
#   fnr_header <record> <field>                 -> a header field's value
#   fnr_emit_header <out> <pc> <n_defects> <n_suites> <tree-commit>
#   fnr_emit_run <out> <defect> <suites> <verdict>
#   fnr_emit_red <out> <defect> <suite> <text>
#   fnr_validate <record> <defects-file> <texts-file> <workdir>
#   fnr_compare <live-record> <committed-record>
#
#   facility_negctl_record.sh compare <live> <committed>
#
# ---------------------------------------------------------------------------

FNR_RECORD_BASENAME="facility_negctl_observed.tsv"

# fnr_record_path <tests-qemu-dir>
fnr_record_path() {
    printf '%s\n' "$1/$FNR_RECORD_BASENAME"
}

# fnr_header <record> <field>  -- first match only; empty when absent.
fnr_header() {
    sed -n "s/^# $2:[ 	]*//p" "$1" 2>/dev/null | head -1
}

# fnr_emit_header <out> <positive-control> <defects-in-manifest> <suites-derived> <tree-commit>
#
# Truncates <out>. The prose is part of the artifact on purpose: this file gets
# read by whoever is holding a diff that shrinks it, and the sentence it must
# not be read past is the one at the top.
fnr_emit_header() {
    cat >"$1" <<EOF
# tests/qemu/facility_negctl_observed.tsv
#
# EXECUTION-SOURCED ATTRIBUTION RECORD. Emitted by
# tests/qemu/run_facility_negctl.sh, which is the only thing in this program
# that executes anything: it boots QEMU against a real /dev/vms and injects
# each defect in tests/qemu/facility_defects.sh one at a time.
#
# WHAT THIS PROVES ON A HOST THAT DID NOT RUN THE DRIVER:
#
#   A PAST RUN, ON THE TREE NAMED BELOW, OBSERVED THESE RESULTS.
#   IT DOES NOT SAY THEY HOLD NOW.
#
# This is a snapshot and a file in the repo: a row written by hand reads
# exactly like an observed one to every static reader. What it is not free to
# be is WRONG -- the driver re-emits this record on every CI run and compares
# it row for row with this committed copy, and any disagreement is red.
#
# DO NOT HAND-EDIT. Regenerate it from the driver's CI job output. Rows are
# one observed failing assertion each, so shrinking this file is proportional
# to what is being deleted, and visible.
#
# Row shapes (TAB-separated, exactly four fields):
#   RUN <defect> <suites that went red, space-joined, or "-"> <pass|fail>
#   RED <defect> <suite or (harness)> <assertion text observed FAILing>
#
# generated-at: $(date -u +%Y-%m-%dT%H:%M:%SZ)
# tree-commit: $5
# positive-control: $2
# defects-in-manifest: $3
# suites-derived: $4
EOF
}

# fnr_emit_run <out> <defect> <suites-space-joined-or-dash> <pass|fail>
fnr_emit_run() {
    printf 'RUN\t%s\t%s\t%s\n' "$2" "$3" "$4" >>"$1"
}

# fnr_emit_red <out> <defect> <suite> <assertion text>
fnr_emit_red() {
    printf 'RED\t%s\t%s\t%s\n' "$2" "$3" "$4" >>"$1"
}

# ---------------------------------------------------------------------------
# fnr_emit_defect <out> <defect> <fail-map> <pass|fail>
#
# One defect's whole contribution, written from the driver's own fail_map():
# "<suite-or-(harness)>\t<assertion text>", one line per DISTINCT observed
# failing assertion. This is the ONLY translation step between what the QEMU
# run printed and what the record says, so it lives here rather than inline in
# the driver -- inline, nothing could exercise it without booting QEMU, and an
# untested translation between an execution and the record of it is the one
# place this whole mechanism could quietly lie.
#
# "(harness)" is fail_map()'s bucket for a FAIL line printed outside any suite
# banner. It is EXCLUDED from the RUN row's suite list, because it is not a
# suite and a suite population must not contain one, and KEPT as a RED row,
# because it is a real observation and dropping it would make the record say
# less than the run did.
# ---------------------------------------------------------------------------
fnr_emit_defect() {
    _ed_out="$1"; _ed_d="$2"; _ed_map="$3"; _ed_v="$4"
    _ed_suites=$(cut -f1 "$_ed_map" 2>/dev/null | sort -u | grep -vFx '(harness)' | tr '\n' ' ')
    _ed_suites=$(printf '%s' "$_ed_suites" | sed 's/ *$//')
    [ -n "$_ed_suites" ] || _ed_suites="-"
    fnr_emit_run "$_ed_out" "$_ed_d" "$_ed_suites" "$_ed_v"
    while IFS="$(printf '\t')" read -r _ed_s _ed_t; do
        [ -n "$_ed_t" ] || continue
        fnr_emit_red "$_ed_out" "$_ed_d" "$_ed_s" "$_ed_t"
    done <"$_ed_map"
}

# ---------------------------------------------------------------------------
# fnr_validate <record> <defects-file> <texts-file> <workdir>
#
#   defects-file  one CURRENT manifest defect name per line.
#   texts-file    "<defect>\t<assertion text>" for every require_fail and
#                 knock_on_fail the CURRENT manifest names. Supplied by the
#                 caller because the two callers reach the manifest
#                 differently (facility_defects.sh has defect_field in scope;
#                 the driver shells out to it).
#
# Returns
#   0  usable -- the derived files below are populated
#   2  REFUSAL -- printed, and nothing may be certified from this record
#
# Leaves in <workdir>:
#   fnr_run          RUN rows
#   fnr_red          RED rows
#   fnr_exec         defects with a RUN row AND still in the manifest
#   fnr_exec_pass    of those, the ones whose RUN verdict is `pass`
#   fnr_complete     of those, the ones whose manifest texts are ALL present as
#                    RED rows -- the only ones a "proven" claim may cover
#   fnr_incomplete   executed+pass, but the manifest names texts this record
#                    does not carry (the manifest grew since the run)
#   fnr_unproven     manifest defects with no RUN row at all
#   fnr_red_suites   distinct suites with at least one RED row, "(harness)"
#                    excluded (it is not a suite)
#
# THE ASYMMETRY, STATED ONCE. Contradiction refuses; incompleteness demotes.
#   * a defect or an assertion text in the RECORD that the MANIFEST does not
#     have  ->  REFUSAL. The record asserts something about a tree that is not
#     this one. This is the deletion case, and it is the case this whole
#     mechanism exists for.
#   * a defect or an assertion text in the MANIFEST that the RECORD does not
#     have  ->  counted, printed, and excluded from every proven population.
#     Adding a defect is correct maintenance; refusing on it would red correct
#     code, and a gate that reds on correct code is the one the next person
#     weakens.
# ---------------------------------------------------------------------------
fnr_validate() {
    _fv_rec="$1"; _fv_defs="$2"; _fv_txt="$3"; _fv_w="$4"

    : >"$_fv_w/fnr_run"; : >"$_fv_w/fnr_red"
    : >"$_fv_w/fnr_exec"; : >"$_fv_w/fnr_exec_pass"
    : >"$_fv_w/fnr_complete"; : >"$_fv_w/fnr_incomplete"
    : >"$_fv_w/fnr_unproven"; : >"$_fv_w/fnr_red_suites"

    # --- REFUSAL 1: the record is not there / has no rows -------------------
    if [ ! -f "$_fv_rec" ]; then
        echo "FAIL: REFUSING to read an execution record: no file at $_fv_rec"
        return 2
    fi
    grep -v '^#' "$_fv_rec" | grep -v '^[ 	]*$' >"$_fv_w/fnr_rows" || true
    if [ ! -s "$_fv_w/fnr_rows" ]; then
        echo "FAIL: REFUSING to certify from $_fv_rec: it has ZERO data rows."
        echo "  -> a header with no rows records no observation. It is not the same"
        echo "     file as a missing one and it is the same amount of evidence."
        return 2
    fi

    # --- REFUSAL 2: row shape ----------------------------------------------
    _fv_bad=$(awk -F'\t' '
        NF != 4 { print "wrong field count (" NF ", want 4): " $0; next }
        $1 != "RUN" && $1 != "RED" { print "column 1 must be RUN or RED: " $0; next }
        $2 == "" { print "empty defect name in column 2: " $0; next }
        $3 == "" { print "empty column 3: " $0; next }
        $4 == "" { print "empty column 4: " $0; next }
        $1 == "RUN" && $4 != "pass" && $4 != "fail" {
            print "a RUN row verdict must be pass|fail: " $0; next }
    ' "$_fv_w/fnr_rows")
    if [ -n "$_fv_bad" ]; then
        echo "FAIL: REFUSING to certify from $_fv_rec: malformed row(s)."
        printf '%s\n' "$_fv_bad" | sed 's/^/    /'
        echo "  -> a row this reader cannot parse is refused, never skipped: skipping"
        echo "     is how a record becomes an allowlist with a typo in it."
        return 2
    fi

    awk -F'\t' '$1 == "RUN"' "$_fv_w/fnr_rows" >"$_fv_w/fnr_run"
    awk -F'\t' '$1 == "RED"' "$_fv_w/fnr_rows" >"$_fv_w/fnr_red"

    if [ ! -s "$_fv_w/fnr_run" ]; then
        echo "FAIL: REFUSING to certify from $_fv_rec: it carries no RUN row, so it"
        echo "      records no defect as having been executed at all."
        return 2
    fi

    # --- REFUSAL 3: a defect recorded twice --------------------------------
    _fv_dups=$(cut -f2 "$_fv_w/fnr_run" | sort | uniq -d)
    if [ -n "$_fv_dups" ]; then
        echo "FAIL: REFUSING to certify from $_fv_rec: defect(s) with two RUN rows:"
        printf '%s\n' "$_fv_dups" | sed 's/^/    /'
        echo "  -> two rows for one defect make its verdict depend on read order."
        return 2
    fi

    # --- REFUSAL 4: the header ---------------------------------------------
    _fv_stamp=$(fnr_header "$_fv_rec" generated-at)
    case "$_fv_stamp" in
        [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z) ;;
        *)
            echo "FAIL: REFUSING to certify from $_fv_rec: no generated-at stamp"
            echo "      (it carries '${_fv_stamp:-(nothing)}' where YYYY-MM-DDTHH:MM:SSZ belongs)."
            echo "  -> this record is a SNAPSHOT, and a snapshot whose age cannot be"
            echo "     printed cannot be judged."
            return 2;;
    esac
    _fv_then=$(date -u -d "$_fv_stamp" +%s 2>/dev/null || true)
    _fv_now=$(date -u +%s)
    if [ -z "$_fv_then" ]; then
        echo "FAIL: REFUSING to certify from $_fv_rec: generated-at '$_fv_stamp' has the"
        echo "      right SHAPE but date(1) cannot read it as an instant."
        return 2
    fi
    if [ "$_fv_then" -gt "$_fv_now" ]; then
        echo "FAIL: REFUSING to certify from $_fv_rec: generated-at is in the FUTURE"
        echo "      ($_fv_stamp). A record of a run cannot predate the run."
        echo "  -> the age this gate prints is the only disclosure of staleness, and a"
        echo "     negative age discloses nothing."
        return 2
    fi
    _fv_commit=$(fnr_header "$_fv_rec" tree-commit)
    if [ -z "$_fv_commit" ]; then
        echo "FAIL: REFUSING to certify from $_fv_rec: no tree-commit header."
        echo "  -> a record that does not say which tree it was produced from cannot be"
        echo "     compared with any tree, including this one."
        return 2
    fi
    _fv_pc=$(fnr_header "$_fv_rec" positive-control)
    if [ "$_fv_pc" != "pass" ]; then
        echo "FAIL: REFUSING to certify from $_fv_rec: positive-control is"
        echo "      '${_fv_pc:-(absent)}', not 'pass'."
        echo "  -> the driver runs a pristine positive control FIRST precisely so that a"
        echo "     harness failing indiscriminately cannot certify anything. A record"
        echo "     emitted without that guarantee is unfounded, whatever its rows say."
        return 2
    fi

    # --- REFUSAL 5: a RED row for a defect with no RUN row -----------------
    cut -f2 "$_fv_w/fnr_run" | sort -u >"$_fv_w/fnr_run_names"
    cut -f2 "$_fv_w/fnr_red" | sort -u >"$_fv_w/fnr_red_names"
    _fv_orph=$(comm -13 "$_fv_w/fnr_run_names" "$_fv_w/fnr_red_names")
    if [ -n "$_fv_orph" ]; then
        echo "FAIL: REFUSING to certify from $_fv_rec: RED row(s) for defect(s) with no"
        echo "      RUN row -- the record contradicts itself:"
        printf '%s\n' "$_fv_orph" | sed 's/^/    /'
        return 2
    fi

    # --- REFUSAL 6: the record names a defect THIS MANIFEST DOES NOT HAVE --
    # The deletion smoking gun, and the reason this file exists. A record
    # cannot be about a manifest that has since lost the entry it names.
    sort -u "$_fv_defs" | grep -v '^[ 	]*$' >"$_fv_w/fnr_manifest" || true
    _fv_gone=$(comm -23 "$_fv_w/fnr_run_names" "$_fv_w/fnr_manifest")
    if [ -n "$_fv_gone" ]; then
        echo "FAIL: REFUSING to certify from $_fv_rec: it records defect(s) the manifest"
        echo "      no longer has:"
        printf '%s\n' "$_fv_gone" | sed 's/^/    /'
        echo "  -> a past run executed these; DEFECTS does not list them now. Either the"
        echo "     entry was deleted -- in which case say so in a commit that argues it,"
        echo "     and regenerate this record from a CI run of the smaller manifest --"
        echo "     or the deletion was an accident. This gate will not guess, and it"
        echo "     will not certify a count from a record about a different manifest."
        return 2
    fi

    # --- REFUSAL 7: an observed assertion text the manifest does not name --
    # The same rule one level down. If require_fail/knock_on_fail texts were
    # edited after the run, the record's observations are about strings this
    # tree no longer contains, and "PROVEN able to go red" over them would be
    # a claim about deleted text.
    sort -u "$_fv_txt" | grep -v '^[ 	]*$' >"$_fv_w/fnr_txt_manifest" || true
    cut -f2,4 "$_fv_w/fnr_red" | sort -u >"$_fv_w/fnr_txt_observed"
    _fv_txtgone=$(comm -13 "$_fv_w/fnr_txt_manifest" "$_fv_w/fnr_txt_observed")
    if [ -n "$_fv_txtgone" ]; then
        echo "FAIL: REFUSING to certify from $_fv_rec: it records assertion text(s) that"
        echo "      the manifest's require_fail/knock_on_fail no longer name:"
        printf '%s\n' "$_fv_txtgone" | sed 's/^/    /' | head -20
        echo "  -> the record observed these FAILing; the manifest has since been"
        echo "     reworded or trimmed. Regenerate the record from a CI run against the"
        echo "     current manifest. Do not resolve this by editing the record: it is"
        echo "     the observation, not the claim."
        return 2
    fi

    # --- usable. Now the populations, all DERIVED --------------------------
    comm -12 "$_fv_w/fnr_run_names" "$_fv_w/fnr_manifest" >"$_fv_w/fnr_exec"
    awk -F'\t' '$1 == "RUN" && $4 == "pass" { print $2 }' "$_fv_w/fnr_run" \
        | sort -u >"$_fv_w/fnr_pass_names"
    comm -12 "$_fv_w/fnr_exec" "$_fv_w/fnr_pass_names" >"$_fv_w/fnr_exec_pass"
    comm -13 "$_fv_w/fnr_run_names" "$_fv_w/fnr_manifest" >"$_fv_w/fnr_unproven"

    # A defect is COMPLETE when every assertion text the manifest names for it
    # appears as a RED row of this record. Incomplete means the manifest grew
    # since the run: the defect still counts as executed, but no "proven"
    # population may cover it.
    # ONE awk over the three sorted-unique files, not two subprocesses per
    # defect: this runs inside `coverage`, which runs inside the ordinary ctest
    # job, and a per-defect fan-out here showed up as whole seconds.
    # Both text files are `sort -u`, so counting matched keys against the
    # manifest's own per-defect total is exact set containment.
    awk -F'\t' -v C="$_fv_w/fnr_complete" -v I="$_fv_w/fnr_incomplete" '
        FILENAME == ARGV[1] { total[$1]++; key[$1 SUBSEP $2] = 1; next }
        FILENAME == ARGV[2] { if (($1 SUBSEP $2) in key) seen[$1]++; next }
        { if ((seen[$0] + 0) >= (total[$0] + 0)) print $0 > C; else print $0 > I }
    ' "$_fv_w/fnr_txt_manifest" "$_fv_w/fnr_txt_observed" "$_fv_w/fnr_exec_pass"
    [ -f "$_fv_w/fnr_complete" ] || : >"$_fv_w/fnr_complete"
    [ -f "$_fv_w/fnr_incomplete" ] || : >"$_fv_w/fnr_incomplete"

    # Suites a COMPLETE defect's run actually saw an assertion fail in. Only
    # complete+pass defects contribute: a defect whose control FAILED observed
    # a red set the driver itself rejected, and a partial observation cannot
    # certify a suite.
    awk -F'\t' 'NR == FNR { ok[$0] = 1; next } ($2 in ok) && $3 != "(harness)" { print $3 }' \
        "$_fv_w/fnr_complete" "$_fv_w/fnr_red" | sort -u >"$_fv_w/fnr_red_suites"

    return 0
}

# ---------------------------------------------------------------------------
# fnr_compare <live-record> <committed-record>
#
# The part the deleter does not own. In CI the driver runs anyway, so the
# freshly observed rows and the committed rows can be required to be THE SAME
# SET. Both directions fail, and they fail for different reasons:
#
#   only in LIVE       the committed record is BEHIND what the tree now does.
#                      Regenerate and commit it.
#   only in COMMITTED  the committed record claims an observation this run did
#                      NOT make. That is either a shrunken manifest whose
#                      record was left behind, or a row that was written rather
#                      than observed.
#
# Headers are deliberately NOT compared: generated-at and tree-commit differ on
# every run by construction, and comparing them would make this always red,
# which is the same as never red.
#
# Returns 0 identical, 1 different (diagnostics printed).
# ---------------------------------------------------------------------------
fnr_compare() {
    _fc_live="$1"; _fc_com="$2"
    _fc_w=$(mktemp -d) || return 1
    grep -v '^#' "$_fc_live" 2>/dev/null | grep -v '^[ 	]*$' | sort -u >"$_fc_w/live"
    grep -v '^#' "$_fc_com"  2>/dev/null | grep -v '^[ 	]*$' | sort -u >"$_fc_w/com"
    _fc_onlylive=$(comm -23 "$_fc_w/live" "$_fc_w/com")
    _fc_onlycom=$(comm -13 "$_fc_w/live" "$_fc_w/com")
    rm -rf "$_fc_w"
    if [ -z "$_fc_onlylive" ] && [ -z "$_fc_onlycom" ]; then
        return 0
    fi
    echo "  FAIL: the committed execution record does not match what this run observed."
    if [ -n "$_fc_onlylive" ]; then
        echo "        OBSERVED NOW but not in the committed record -- it is BEHIND the tree:"
        printf '%s\n' "$_fc_onlylive" | sed 's/^/          + /' | head -40
    fi
    if [ -n "$_fc_onlycom" ]; then
        echo "        IN THE COMMITTED RECORD but NOT observed now -- it claims an"
        echo "        observation this run did not make:"
        printf '%s\n' "$_fc_onlycom" | sed 's/^/          - /' | head -40
    fi
    echo "        -> the committed record is the static gates' only execution-sourced"
    echo "           evidence. Regenerate it from THIS job's emitted record and commit"
    echo "           it; do not edit it to agree."
    return 1
}

# Runnable for the whole-file operation the CI job needs.
#
# GUARDED ON $0, not on $1. This file is SOURCED by facility_defects.sh and by
# run_facility_negctl.sh, and a sourced file sees its SOURCER's positional
# parameters -- so dispatching on $1 would make
# `run_facility_negctl.sh compare ...` (a defect list, in that script's
# grammar) silently run this dispatcher instead. Basename of $0 is the only
# thing here that distinguishes "executed" from "sourced".
case "$(basename "${0:-}")" in
    facility_negctl_record.sh)
        case "${1:-}" in
            compare) shift; fnr_compare "$@";;
            '') ;;
            *) echo "usage: facility_negctl_record.sh compare <live> <committed>" >&2
               exit 2;;
        esac
        ;;
esac
