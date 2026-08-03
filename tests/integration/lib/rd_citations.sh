# rd_citations.sh - SOURCEABLE helper (rd vms-8cc). Not a test; nothing here
# runs at source time.
#
# WHAT IT IS FOR. The vms_kif caller census sells an exemption in exchange for
# a CITED rd ITEM:
#
#     OVMX-UNWIRED: vms_kif_foo (vms-xxx) -- why
#
# Until vms-8cc it validated only the SHAPE of the id. MEASURED, on the
# revision this replaces: one declaration repointed at `vms-q9z9`, an id that
# has never existed, left the census rc=0 with its counts unchanged; one
# repointed at `vms-fb9`, whose status is `done`, did the same. The exemption
# cost a well-formed token.
#
# WHO ACTUALLY CALLS THIS. tests/integration/test_kif_caller_census.sh, and
# tests/integration/test_rd_citations_fresh.sh for its own ledger self-check.
# That is the whole list, and it is checkable:
#
#     grep -rn 'rd_cite_check' tests/
#
# An earlier revision of this header said the userspace service register
# (OVMX-USERSPACE / OVMX-PARTIAL / OVMX-LOCAL / OVMX-EXECUTIVE) was checked
# here too. IT IS NOT: test_userspace_service_register.sh contains no reference
# to this library, to rd_cite_check, or to the ledger -- the grep above returns
# nothing for it. Those declarations are still shape-checked only. That is
# tracked as rd vms-fab (85 of its 88 declarations cite CLOSED items, so wiring
# the register to this checker reds it until the citations are repointed) and
# is deliberately NOT done here.
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
#         - THE COUNT HAS A FLOOR (rd vms-279 shape). This library rescans the
#           tree itself and refuses when that scan finds NO cited id under src/
#           or tools/ at all. MEASURED on the revision this replaces, by
#           calling the old rd_cite_check directly on a tree with a header-only
#           ledger and an empty id list: rc=0, summary "0 declaration site(s)
#           cite 0 distinct rd item(s) -- 0 open, 0 closed, 0 unknown to rd".
#           A gate that counts things and treats zero as success is satisfied
#           by deletion. MEASURED after: stripping every data row from the
#           committed ledger took the census from rc=0 PASS to rc=1 REFUSING.
#         - EVERY id the tree cites must have a ledger row, whether or not the
#           CALLER passed that id in. The caller's id list is the caller's own
#           parse; this scan is independent of it, so a citation the caller
#           does not parse cannot go unresolved.
#         - a stamp in the FUTURE is a REFUSAL. MEASURED against the revision
#           this replaces: future-dating the stamp to 2999-12-31T00:00:00Z made
#           this function print "generated 2999-12-31T00:00:00Z (-355530
#           day(s) old)" and return 0. An impossible age printed as fact is
#           worse than no age: the age is the ONLY disclosure of how stale the
#           snapshot is, so a number that cannot be true discloses nothing.
#   DOES  - the ledger is a SNAPSHOT. An item closed in rd after the stamp
#   NOT     still reads `open` here until somebody regenerates. That window is
#           real; it is disclosed in the summary this library prints, and
#           tests/integration/test_rd_citations_fresh.sh re-derives the ledger
#           from live rd and reds when the two disagree. That test needs rd, so
#           it SKIPS in CI -- meaning the drift window is closed on the dev
#           host and only there.
#         - bound the age on the OLD side. A stamp from last year prints its
#           real age and passes. Choosing a maximum age would be a policy
#           invented here; the age is printed instead, every run.
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
#           2  the ledger is missing, malformed, empty, stamped in the future,
#              or the tree cites nothing at all -- a REFUSAL to measure
#
#   Failure diagnostics are printed as they are found. The caller is expected
#   to fold a non-zero return into its own status; there is no silent path.

RD_CITE_LEDGER_REL="tracking/rd-citations.tsv"

# rd_cite_ledger_path <src_root>
rd_cite_ledger_path() {
    printf '%s\n' "$1/$RD_CITE_LEDGER_REL"
}

# The dirs and file suffixes a declaration can live in. These MIRROR
# tools/gen_rd_citations.py (SCAN_DIRS / SCAN_SUFFIXES) on purpose: this scan
# exists to be a SECOND opinion on the same universe, so it has to cover the
# same universe. Verified by rd_cite_scan_tree's own count against the
# generator's: both read 101 declaration lines carrying an id, 12 distinct ids,
# on the tree at the time of writing.
RD_CITE_SCAN_DIRS="src tools"

# "src/, tools/" -- for the diagnostics, so the printed floor names the tree it
# was derived from rather than leaving the reader to guess.
rd_cite_scan_dirs_label() {
    _sd_out=""
    for _sd_d in $RD_CITE_SCAN_DIRS; do
        if [ -z "$_sd_out" ]; then _sd_out="$_sd_d/"; else _sd_out="$_sd_out, $_sd_d/"; fi
    done
    printf '%s\n' "$_sd_out"
}

# rd_cite_scan_tree <src_root> <workdir>
#
# Rescan the tree for OVMX-<TOKEN>: declarations WITHOUT asking the caller and
# WITHOUT running the generator. Leaves three files in <workdir>:
#
#   cite_tree_markers  every "path:lineno:text" carrying an OVMX-<TOKEN>: marker
#   cite_tree_sites    the subset of those that also carry an rd id
#   cite_tree_ids      the distinct ids, sorted
#
# WHY THIS IS NOT REDUNDANT WITH THE CALLER'S LIST. The caller parses the
# declarations IT understands and hands the ids over; this gate then answers
# about exactly those. That makes the caller's parser the floor on what gets
# checked, and a parser that finds nothing produces a gate that certifies
# nothing while printing a PASS. This scan is the independent floor.
rd_cite_scan_tree() {
    _ts_root="$1"; _ts_work="$2"
    : > "$_ts_work/cite_tree_markers"
    for _ts_d in $RD_CITE_SCAN_DIRS; do
        [ -d "$_ts_root/$_ts_d" ] || continue
        find "$_ts_root/$_ts_d" -type f \
            \( -name '*.c' -o -name '*.h' -o -name '*.inc' -o -name '*.S' \
               -o -name '*.py' -o -name '*.sh' -o -name '*.cmake' \
               -o -name '*.txt' -o -name '*.md' \) -print
    done > "$_ts_work/cite_tree_files"
    if [ -s "$_ts_work/cite_tree_files" ]; then
        # -H so a single-file tree still reports paths; || true because grep
        # exits 1 on "no matches", which is a real answer here, not an error.
        tr '\n' '\0' < "$_ts_work/cite_tree_files" \
            | xargs -0 grep -Hn 'OVMX-[A-Z][A-Z]*:' \
            > "$_ts_work/cite_tree_markers" 2>/dev/null || true
    fi
    grep -E '\<vms-[0-9a-z]+(\.[0-9a-z]+)?\>' "$_ts_work/cite_tree_markers" \
        > "$_ts_work/cite_tree_sites" 2>/dev/null || true
    grep -oE '\<vms-[0-9a-z]+(\.[0-9a-z]+)?\>' "$_ts_work/cite_tree_sites" 2>/dev/null \
        | sort -u > "$_ts_work/cite_tree_ids" || : > "$_ts_work/cite_tree_ids"
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
    # REFUSAL 2b: the stamp must be a REAL instant, NOT IN THE FUTURE.
    #
    # The shape check above accepts 2999-12-31T00:00:00Z and 2026-13-45. The
    # age arithmetic below then printed "(-355531 day(s) old)" and returned 0,
    # MEASURED. The stamp is attacker-controlled text in a file, and an age
    # computed from it is only evidence if the instant is possible: a ledger
    # cannot have been derived from rd later than now, so a future stamp means
    # the field was written rather than generated. Refuse instead of printing
    # an impossible number as a fact.
    #
    # The OLD side is deliberately not bounded here -- see the header. A real
    # old stamp prints its real age.
    # -----------------------------------------------------------------------
    _cs_then=$(date -u -d "$_cs_stamp" +%s 2>/dev/null || true)
    _cs_now=$(date -u +%s)
    if [ -z "$_cs_then" ]; then
        echo "FAIL: REFUSING to certify citations: generated-at stamp is not a real instant"
        echo "      $_cs_rel carries '$_cs_stamp', which has the right SHAPE but"
        echo "      date(1) cannot read it as a time."
        echo "  -> regenerate it:  tools/gen_rd_citations.py"
        printf '  citations: NOT CHECKED -- %s\n' "$_cs_rel stamp '$_cs_stamp' is not a real instant" \
            >> "$_cs_work/cite_summary"
        return 2
    fi
    if [ "$_cs_then" -gt "$_cs_now" ]; then
        echo "FAIL: REFUSING to certify citations: generated-at stamp is in the FUTURE"
        echo "      $_cs_rel says it was generated at $_cs_stamp, which has not"
        echo "      happened yet. A snapshot of rd cannot predate its own source."
        echo "  -> regenerate it on a host with a correct clock:  tools/gen_rd_citations.py"
        echo "     This is a refusal and not a warning because the age this gate"
        echo "     prints every run is the only disclosure of the snapshot's"
        echo "     staleness, and a negative age discloses nothing."
        printf '  citations: NOT CHECKED -- %s\n' "$_cs_rel is stamped in the future ($_cs_stamp)" \
            >> "$_cs_work/cite_summary"
        return 2
    fi

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
    # THE FLOOR (rd vms-279 shape). Everything above this point checks the
    # QUALITY of what it was given. Nothing above it checks that it was given
    # anything -- and MEASURED on the revision this replaces, the old
    # rd_cite_check called on a header-only ledger with an empty id list
    # returned 0 and printed
    #
    #     0 declaration site(s) cite 0 distinct rd item(s)
    #
    # as its summary. A count with no floor can be driven to zero by deletion,
    # and deletion is always the cheapest way to satisfy a counter.
    #
    # So the floor is DERIVED HERE, from the tree, by a scan that does not go
    # through the caller and does not go through the generator, and it is
    # PRINTED in the summary every run whether or not it fires.
    # -----------------------------------------------------------------------
    rd_cite_scan_tree "$_cs_root" "$_cs_work"
    _cs_tree_markers=$(grep -c . "$_cs_work/cite_tree_markers" 2>/dev/null || true)
    _cs_tree_sites=$(grep -c . "$_cs_work/cite_tree_sites" 2>/dev/null || true)
    _cs_tree_nids=$(grep -c . "$_cs_work/cite_tree_ids" 2>/dev/null || true)
    _cs_rows_n=$(grep -c . "$_cs_work/cite_rows" 2>/dev/null || true)
    : "${_cs_tree_markers:=0}" "${_cs_tree_sites:=0}" "${_cs_tree_nids:=0}" "${_cs_rows_n:=0}"

    # FLOOR 1: the tree cites nothing at all.
    if [ "$_cs_tree_nids" -eq 0 ]; then
        echo "FAIL: REFUSING to certify citations: this scan found NO rd id cited by any"
        echo "      OVMX-<TOKEN>: declaration under $(rd_cite_scan_dirs_label)"
        echo "      ($_cs_tree_markers marker line(s) seen, $_cs_tree_sites of them carrying an id.)"
        echo "  -> Two things look identical from here and this gate cannot tell them"
        echo "     apart, so it refuses rather than guess:"
        echo "       (a) every facility got wired and no declaration is needed any more"
        echo "           -- a large, deliberate, announceable change; or"
        echo "       (b) the declarations were deleted, which is how a gate that counts"
        echo "           citations is satisfied for free."
        echo "     If (a) is the true one -- if OVMX genuinely declares nothing -- say so"
        echo "     by editing this floor, in a commit that argues it. Do not reach it by"
        echo "     deletion."
        printf '  citations: NOT CHECKED -- %s\n' \
            "no OVMX-<TOKEN>: declaration under the scanned tree cites any rd id" \
            >> "$_cs_work/cite_summary"
        return 2
    fi

    # FLOOR 2: the tree cites ids, and the ledger has no rows to resolve them.
    if [ "$_cs_rows_n" -eq 0 ]; then
        echo "FAIL: REFUSING to certify citations: the tree cites $_cs_tree_nids rd id(s) and"
        echo "      $_cs_rel has ZERO data rows."
        echo "  -> a ledger with a header and no rows resolves nothing. It is not the"
        echo "     same file as a missing one and it is the same amount of evidence."
        echo "     Regenerate it on a host that has rd:  tools/gen_rd_citations.py"
        printf '  citations: NOT CHECKED -- %s\n' "$_cs_rel has no data rows" \
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

    # -----------------------------------------------------------------------
    # COMPLETENESS, independent of the caller. Everything above answers about
    # the ids the CALLER handed over. This answers about the ids the TREE
    # carries: every one of them must have a row, or the ledger is a partial
    # resolution being read as a total one. It is a RED and not a refusal --
    # the ledger is readable, it is just incomplete, and the fix is a
    # regenerate.
    # -----------------------------------------------------------------------
    _cs_n_treeunres=0
    while IFS= read -r _cs_tid; do
        [ -n "$_cs_tid" ] || continue
        if ! awk -F'\t' -v id="$_cs_tid" '$1 == id { found=1; exit } END { exit !found }' \
                "$_cs_work/cite_rows"; then
            _cs_n_treeunres=$((_cs_n_treeunres + 1))
            echo "FAIL: the tree cites $_cs_tid and $_cs_rel has NO ROW for it"
            grep -n "\<$_cs_tid\>" "$_cs_work/cite_tree_sites" 2>/dev/null \
                | cut -d: -f2- | head -3 | sed 's/^/        /'
            echo "  -> this scan reads the same tree the generator does, so an id it"
            echo "     finds and the ledger does not carry means the ledger was not"
            echo "     regenerated after the citation changed. Run"
            echo "     tools/gen_rd_citations.py on a host with rd and commit the result."
            _cs_rc=1
        fi
    done < "$_cs_work/cite_tree_ids"

    _cs_ncited=$(grep -c . "$_cs_work/cite_want" || true)
    # _cs_then / _cs_now were computed and bounded by REFUSAL 2b above; the age
    # here is arithmetic on an instant already proven to be real and past.
    _cs_age=$(( (_cs_now - _cs_then) / 86400 ))
    _cs_age=" (${_cs_age} day(s) old)"

    {
        echo "  citations: $_cs_nsites declaration site(s) cite $_cs_ncited distinct rd item(s) —"
        echo "          $_cs_n_open open, $_cs_n_closed closed, $_cs_n_absent unknown to rd,"
        echo "          $_cs_n_unlisted not resolved at all, per $_cs_rel"
        echo "          generated $_cs_stamp$_cs_age."
        echo "          FLOOR, rescanned here and not taken from the caller:"
        echo "          $_cs_tree_markers OVMX-<TOKEN>: marker line(s) under $(rd_cite_scan_dirs_label),"
        echo "          $_cs_tree_sites of them citing an id, $_cs_tree_nids distinct id(s),"
        echo "          resolved by $_cs_rows_n ledger row(s); $_cs_n_treeunres tree id(s) unresolved."
        echo "          Zero cited ids is a REFUSAL here, not a pass: a counter with no"
        echo "          floor is satisfied by deleting the things it counts."
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
