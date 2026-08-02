# rd_citations.sh - SOURCEABLE helper (rd vms-8cc). Not a test; nothing here
# runs at source time.
#
# WHAT IT IS FOR. Several standing gates sell an exemption in exchange for a
# CITED rd ITEM: the vms_kif caller census takes
#
#     OVMX-UNWIRED: vms_kif_foo (vms-xxx) -- why
#
# and the userspace service register takes OVMX-USERSPACE / OVMX-PARTIAL /
# OVMX-LOCAL / OVMX-EXECUTIVE in the same shape. Until vms-8cc, both validated
# only the SHAPE of the id. MEASURED, on the revision this replaces: one
# declaration repointed at `vms-q9z9`, an id that has never existed, left the
# census rc=0 with its counts unchanged; one repointed at `vms-fb9`, whose
# status is `done`, did the same. The exemption cost a well-formed token.
#
# WHY THE CHECK IS NOT A LIVE QUERY. rd is nostr-backed and is not reachable
# from CI -- that constraint is why the gates were written to shape-check in
# the first place. So the resolution is done on a host that HAS rd, by
# tools/gen_rd_citations.py, and committed as tracking/rd-citations.tsv. This
# library reads that ledger with nothing but grep and awk, and needs neither rd
# nor a network.
#
# WHAT THAT BUYS AND WHAT IT DOES NOT, stated here because the sentence it
# replaces ("it verifies that a human wrote an id down") is the honest
# disclosure that made the defect findable:
#
#   BUYS  - a fabricated id is red: it is in no ledger row, and regenerating
#           the ledger records it `absent` rather than inventing a row.
#         - a closed id is red: the ledger carries rd's own open/closed answer.
#         - a missing or malformed ledger is a REFUSAL, not a skip.
#   DOES  - the ledger is a SNAPSHOT. An item closed in rd after the stamp
#   NOT     still reads `open` here until somebody regenerates. That window is
#           real; it is disclosed in the summary this library prints, and
#           tests/integration/test_rd_citations_fresh.sh re-derives the ledger
#           from live rd and reds when the two disagree. That test needs rd, so
#           it SKIPS in CI -- meaning the drift window is closed on the dev
#           host and only there.
#         - the ledger is a file in the repo, and a row written by hand reads
#           exactly like a derived one to this library. MEASURED, not inferred:
#           a declaration repointed at `vms-q9z9` plus one appended row
#           `vms-q9z9<TAB>open<TAB>active<TAB>...` took the census to rc=0,
#           PASS, 6 cited items all "open". test_rd_citations_fresh.sh caught
#           the same tree at rc=1, naming the row. That is the split: the
#           CI-side check is bought by a forged row; the freshness test is not,
#           and it needs rd.
#
# USAGE
#     . "$(dirname "$0")/lib/rd_citations.sh"
#     rd_cite_check <src_root> <ids_file> <workdir> <n_decl_sites> <what>
#
#   ids_file      one rd id per line; duplicates and blank lines are tolerated.
#   workdir       a scratch dir the caller owns. The summary the caller should
#                 print at the end of its own report is left in
#                 $workdir/cite_summary.
#   n_decl_sites  how many declaration lines produced those ids, for the
#                 summary only.
#   what          a noun for the diagnostics, e.g. "unwired declaration".
#
#   Returns 0  every cited id is present in the ledger and open
#           1  at least one is closed, unknown to rd, or missing from the ledger
#           2  the ledger is missing or malformed -- a REFUSAL to measure
#
#   Failure diagnostics are printed as they are found. The caller is expected
#   to fold a non-zero return into its own status; there is no silent path.

RD_CITE_LEDGER_REL="tracking/rd-citations.tsv"

# rd_cite_ledger_path <src_root>
rd_cite_ledger_path() {
    printf '%s\n' "$1/$RD_CITE_LEDGER_REL"
}

# rd_cite_check <src_root> <ids_file> <workdir> <n_decl_sites> <what>
rd_cite_check() {
    _cs_root="$1"; _cs_ids="$2"; _cs_work="$3"; _cs_nsites="$4"; _cs_what="$5"
    _cs_ledger=$(rd_cite_ledger_path "$_cs_root")
    _cs_rel="$RD_CITE_LEDGER_REL"
    : > "$_cs_work/cite_summary"

    # -----------------------------------------------------------------------
    # REFUSAL 1: there is no ledger.
    #
    # Unconditional -- it fires even when nothing is cited. A gate that
    # measured "0 citations, all fine" against a deleted ledger would be
    # exactly the silent fallback Rule 9 forbids one layer down: deleting the
    # evidence must never be cheaper than fixing the citation.
    # -----------------------------------------------------------------------
    if [ ! -f "$_cs_ledger" ]; then
        echo "FAIL: REFUSING to certify citations: no citation ledger at $_cs_rel"
        echo "  -> regenerate it on a host that has rd:  tools/gen_rd_citations.py"
        echo "     Without it this gate can only check that an id is well FORMED,"
        echo "     which is the defect vms-8cc exists to close: a fabricated id and"
        echo "     a closed id both passed that way."
        printf '  citations: NOT CHECKED -- %s\n' "no ledger at $_cs_rel" \
            >> "$_cs_work/cite_summary"
        return 2
    fi

    # -----------------------------------------------------------------------
    # REFUSAL 2: the stamp. A ledger with no generated-at cannot have its age
    # disclosed, and an undisclosed snapshot age is the whole residual risk of
    # this design.
    # -----------------------------------------------------------------------
    _cs_stamp=$(sed -n 's/^# generated-at:[ \t]*//p' "$_cs_ledger" | head -1)
    case "$_cs_stamp" in
        [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z) ;;
        *)
            echo "FAIL: REFUSING to certify citations: citation ledger has no generated-at stamp"
            echo "      $_cs_rel carries '${_cs_stamp:-(nothing)}' where a"
            echo "      YYYY-MM-DDTHH:MM:SSZ stamp belongs."
            echo "  -> regenerate it:  tools/gen_rd_citations.py"
            echo "     The stamp is not decoration: this ledger is a SNAPSHOT of rd,"
            echo "     and a snapshot whose age cannot be printed cannot be judged."
            printf '  citations: NOT CHECKED -- %s\n' "$_cs_rel has no generated-at stamp" \
                >> "$_cs_work/cite_summary"
            return 2
            ;;
    esac

    # -----------------------------------------------------------------------
    # REFUSAL 3: row shape. Every non-comment line must be
    #     <id> TAB <open|closed|absent> TAB <rd status> TAB <title>
    # A row this reader cannot parse is not skipped -- skipping it is how a
    # ledger becomes an allowlist with a typo in it.
    # -----------------------------------------------------------------------
    grep -v '^#' "$_cs_ledger" | grep -v '^[ 	]*$' > "$_cs_work/cite_rows" || true

    _cs_bad=$(awk -F'\t' '
        NF != 4 { print "wrong field count (" NF ", want 4): " $0; next }
        $1 !~ /^vms-[0-9a-z]+(\.[0-9a-z]+)?$/ { print "bad id in column 1: " $0; next }
        $2 != "open" && $2 != "closed" && $2 != "absent" {
            print "column 2 must be open|closed|absent: " $0; next }
        $3 == "" { print "empty rd status in column 3: " $0; next }
        $4 == "" { print "empty title in column 4: " $0; next }
    ' "$_cs_work/cite_rows")
    if [ -n "$_cs_bad" ]; then
        echo "FAIL: REFUSING to certify citations: malformed citation ledger $_cs_rel"
        printf '%s\n' "$_cs_bad" | sed 's/^/    /'
        echo "  -> regenerate it:  tools/gen_rd_citations.py"
        echo "     It is a DERIVED artifact; a row edited by hand is the one thing"
        echo "     this whole mechanism cannot tell from the truth."
        printf '  citations: NOT CHECKED -- %s\n' "$_cs_rel is malformed" \
            >> "$_cs_work/cite_summary"
        return 2
    fi

    _cs_dups=$(cut -f1 "$_cs_work/cite_rows" | sort | uniq -d)
    if [ -n "$_cs_dups" ]; then
        echo "FAIL: REFUSING to certify citations: item listed twice in the citation ledger"
        printf '%s\n' "$_cs_dups" | sed 's/^/    /'
        echo "  -> two rows for one id make the verdict depend on read order."
        echo "     Regenerate:  tools/gen_rd_citations.py"
        printf '  citations: NOT CHECKED -- %s\n' "$_cs_rel lists an id twice" \
            >> "$_cs_work/cite_summary"
        return 2
    fi

    # -----------------------------------------------------------------------
    # The check itself.
    # -----------------------------------------------------------------------
    sort -u "$_cs_ids" | grep -v '^[ 	]*$' > "$_cs_work/cite_want" || true
    _cs_rc=0
    _cs_n_open=0; _cs_n_closed=0; _cs_n_absent=0; _cs_n_unlisted=0

    while IFS= read -r _cs_id; do
        [ -n "$_cs_id" ] || continue
        _cs_row=$(awk -F'\t' -v id="$_cs_id" '$1 == id { print; exit }' "$_cs_work/cite_rows")
        if [ -z "$_cs_row" ]; then
            _cs_n_unlisted=$((_cs_n_unlisted + 1))
            echo "FAIL: $_cs_what cites $_cs_id, which is NOT IN the citation ledger"
            echo "  -> $_cs_rel resolves every id cited under src/ and tools/. An id"
            echo "     missing from it was never resolved against rd, so this gate has"
            echo "     no evidence the item exists at all. Either the citation is new"
            echo "     and the ledger was not regenerated (tools/gen_rd_citations.py),"
            echo "     or the id is fabricated -- and a fabricated id buying an"
            echo "     exemption is exactly what vms-8cc closes."
            _cs_rc=1
            continue
        fi
        _cs_verdict=$(printf '%s\n' "$_cs_row" | cut -f2)
        _cs_status=$(printf '%s\n' "$_cs_row" | cut -f3)
        _cs_title=$(printf '%s\n' "$_cs_row" | cut -f4)
        case "$_cs_verdict" in
            open)
                _cs_n_open=$((_cs_n_open + 1))
                ;;
            closed)
                _cs_n_closed=$((_cs_n_closed + 1))
                echo "FAIL: $_cs_what cites a CLOSED rd item: $_cs_id (status: $_cs_status)"
                echo "        $_cs_title"
                echo "  -> a closed item tracks nothing. Either the work is genuinely"
                echo "     done -- in which case delete the declaration and wire it --"
                echo "     or repoint the citation at the item that really carries it."
                _cs_rc=1
                ;;
            absent)
                _cs_n_absent=$((_cs_n_absent + 1))
                echo "FAIL: $_cs_what cites an rd item that DOES NOT EXIST: $_cs_id"
                echo "  -> rd has no such item. The ledger records that rather than"
                echo "     inventing a row for it. A well-formed id is not an owner."
                _cs_rc=1
                ;;
        esac
    done < "$_cs_work/cite_want"

    _cs_ncited=$(grep -c . "$_cs_work/cite_want" || true)
    _cs_age=""
    if _cs_then=$(date -u -d "$_cs_stamp" +%s 2>/dev/null) && [ -n "$_cs_then" ]; then
        _cs_now=$(date -u +%s)
        _cs_age=$(( (_cs_now - _cs_then) / 86400 ))
        _cs_age=" (${_cs_age} day(s) old)"
    fi

    {
        echo "  citations: $_cs_nsites declaration site(s) cite $_cs_ncited distinct rd item(s) —"
        echo "          $_cs_n_open open, $_cs_n_closed closed, $_cs_n_absent unknown to rd,"
        echo "          $_cs_n_unlisted not resolved at all, per $_cs_rel"
        echo "          generated $_cs_stamp$_cs_age."
        echo "          THIS GATE DOES NOT REACH rd -- rd is nostr-backed and not"
        echo "          available in CI. It reads that committed ledger, so an item"
        echo "          closed in rd SINCE that stamp still reads open here, and a"
        echo "          ledger row edited by hand instead of regenerated reads as true."
        echo "          test_rd_citations_fresh.sh re-derives the ledger from live rd"
        echo "          and reds on any disagreement; it needs rd, so it SKIPS in CI."
        echo "          Both of those residuals are closed on a host with rd and"
        echo "          nowhere else."
    } >> "$_cs_work/cite_summary"

    return "$_cs_rc"
}
