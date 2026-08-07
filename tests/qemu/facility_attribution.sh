#!/bin/sh
#
# facility_attribution.sh - PER-ASSERTION RUNTIME ATTRIBUTION, at function
#                           granularity (rd vms-38c, vms-d33, vms-2b2)
#
# ===========================================================================
# WHAT THIS ANSWERS, AND WHY NOTHING ELSE IN THE TREE COULD
# ===========================================================================
#
# Three items independently reached the same conclusion after several measured
# adversarial rounds, and this file is the instrument all three named:
#
#   vms-38c  "the proof CALLS the service" is a SYNTACTIC proxy. Measured buy:
#            one `(void)sys$wflor(0u, 0u);` placed AFTER `return` -- statically
#            unreachable dead code, which the compiler is free to delete --
#            satisfies it. Check 4 is bought by code that never runs.
#   vms-d33  the kif census asks "is there a product path", never "does it
#            execute". A fabricated caller genuinely compiled and genuinely
#            reachable from a root still counts. LINKING IS NOT EXECUTING and
#            REACHABLE IS NOT EXECUTED.
#   vms-2b2  only 9 of 33 vms_ioctl_* handlers sit inside any mutation hunk,
#            and that measure is LINE-LEVEL, not behavioural. The honest
#            question -- "which wired handlers have NO mutation that changes
#            what they return" -- "is answerable by execution".
#
# THE TRAP ALL THREE RECORDED, WHICH THIS FILE IS BUILT AROUND:
#
#     "a wrapper that merely records 'was called' is bought by one more
#      ignored call inside the assertion. Coverage must be attributed to what
#      the assertion ASSERTS, not to what it touches."
#
# So this is deliberately NOT a call tracer, NOT a coverage counter and NOT a
# "was reached" marker. Every one of those is satisfied by an ignored call.
#
# ===========================================================================
# THE MECHANISM: THE DEFECT IS THE PROBE
# ===========================================================================
#
# An assertion goes RED when a function is mutated IF AND ONLY IF the value
# that assertion checks depends on that function's behaviour. That is not an
# approximation of "asserts on it" -- it IS "asserts on it", measured:
#
#   - an ignored call -- `(void)sys$wflor(0u, 0u);` -- can never redden an
#     assertion, because nothing reads its result;
#   - dead code after `return` can never redden an assertion, because it never
#     runs;
#   - a call whose result is discarded can never redden an assertion;
#   - a comment, a re-worded assertion text, a manifest field, a declaration,
#     an added file, a CMakeLists line -- none of them can redden an assertion.
#
# Every buy measured on this program across the vms-ecf / vms-e2b / vms-c13
# rounds is inert against this instrument, because every one of them bought a
# STATIC PROPERTY, and this instrument reads only an OBSERVED CHANGE OF
# VERDICT under a mutation that really ran.
#
# The attribution is therefore a join of two halves, and NEITHER half is a
# hand-written field:
#
#   THE SITE HALF  (this file, re-derived every run, never committed)
#       WHICH FUNCTION each defect's injection ACTUALLY MUTATES. Not the
#       manifest's hand-written `targets` (a FILE list, and a human-written
#       one -- the exposure recorded on vms-38c). This runs the REAL
#       `facility_defects.sh apply` against a throwaway copy of the tree,
#       diffs the result against the pristine copy, and reports the enclosing
#       C function of every line the sed actually changed. To move a site you
#       must move a sed anchor; an anchor that does not land is already a
#       BROKEN FIXTURE (cmd_apply, section 4), so a fabricated site is not
#       reachable by editing a field.
#
#   THE RED HALF   (tests/qemu/facility_negctl_observed.tsv)
#       WHICH ASSERTION ACTUALLY WENT RED, in QEMU, against a real /dev/vms,
#       with that defect injected -- emitted by run_facility_negctl.sh, which
#       is the only thing in this program that executes anything, and floored
#       by that driver's row-for-row comparison of the committed record
#       against what it just observed (so the record can be neither fabricated
#       upward nor allowed to go stale).
#
# Joined on the defect name, the two halves give:
#
#     ATTR <suite> <assertion> <file> <function>
#
#     "MUTATING <function> WAS OBSERVED TO CHANGE THE VERDICT OF <assertion>
#      IN <suite>, IN QEMU, AGAINST A REAL /dev/vms."
#
# ===========================================================================
# WHAT THIS DOES NOT PROVE -- stated here, not left to be discovered
# ===========================================================================
#
#  1. IT IS ASYMMETRIC, exactly like the register's check 6. It can PROVE that
#     an assertion depends on a function. It can NEVER prove that it does not.
#     A function nobody has written a defect for produces no ATTR row because
#     NOTHING PROBED IT, not because anything was measured about it. Read a
#     missing row as "unmeasured", never as "independent". Every consumer of
#     this file must print that distinction rather than collapse it -- the
#     collapse is how "9 of 33 handlers" got read as "24 untested handlers",
#     which vms-2b2 explicitly warns against.
#
#  2. IT IS BOUNDED BY THE PROBE SET. The instrument is exactly as fine as the
#     42 defects in facility_defects.sh. Whole functions are unprobed; some
#     defects mutate a shared helper and therefore attribute to that helper
#     rather than to each caller.
#
#  3. THE RED HALF IS A COMMITTED SNAPSHOT. A row written by hand reads like an
#     observed one to this file. What it is not free to be is WRONG: the driver
#     re-emits and compares it row for row on every full CI run. That floor is
#     inherited here, not re-established.
#
#  4. IT SAYS NOTHING ABOUT WHY an assertion reddened. A mutation that breaks a
#     shared prerequisite reddens assertions that do not "check" the mutated
#     behaviour in the sense a reader means. The record's RED rows do not
#     distinguish require_fail from knock_on_fail, so neither does this. That
#     is a real residual and it is named in `caveats`, not papered over.
#
# ===========================================================================
# COMMANDS
# ===========================================================================
#   sites [<defect>...]         SITE rows: the function each injection really
#                               mutates. Re-derived from the tree, every run.
#   attribute [<defect>...]     ATTR rows: the join. Per-assertion, per-suite,
#                               function-granular, execution-sourced.
#   depends <suite> <function>  rc=0 iff some assertion in <suite> was OBSERVED
#                               to change verdict when <function> was mutated.
#                               This is the query a gate asks.
#   functions                   the distinct functions with any ATTR row.
#   handlers <src-root>         vms-2b2's question, answered by execution:
#                               for every vms_ioctl_* handler, whether any
#                               assertion measurably depends on it.
#   caveats                     print the limits above (so a consumer cannot
#                               quote a number without them).
#   selftest                    negative controls for THIS file.
#
# Env:
#   FA_REPO_ROOT   repo root (default: derived from $0)
#   FA_RECORD      the execution record (default: tests/qemu/facility_negctl_observed.tsv)

set -u

FA_SELF_DIR=$(cd "$(dirname "$0")" && pwd)
FA_REPO_ROOT="${FA_REPO_ROOT:-$(cd "$FA_SELF_DIR/../.." && pwd)}"
FA_MANIFEST="$FA_SELF_DIR/facility_defects.sh"
FA_RECORD="${FA_RECORD:-$FA_SELF_DIR/facility_negctl_observed.tsv}"

TAB=$(printf '\t')

fa_die() { echo "facility_attribution.sh: $*" >&2; exit 2; }

# ---------------------------------------------------------------------------
# enclosing_functions <pristine-file> <changed-line>...
#
# Maps line numbers to the C function that contains them.
#
# THE HEURISTIC, AND WHY IT IS SAFE TO USE HERE. A definition opens at a line
# whose first character is not blank and which carries a `(`, and closes at a
# line that is exactly `}`. That is the kernel/OVMX house style throughout
# src/kernel and src/libvms, and this file's `selftest` pins it against the
# real sources rather than trusting it.
#
# THE FAILURE DIRECTION MATTERS AND IT IS THE SAFE ONE. When the heuristic
# cannot name a function it emits `(file-scope)`, which NEVER matches a
# handler name, so a mis-parse can only ever WITHHOLD attribution -- turning a
# claim unpaid, never paying for one. A parser whose errors granted
# attribution would be an oracle; this one's errors only ever refuse.
# ---------------------------------------------------------------------------
enclosing_functions() {
    _file="$1"; shift
    printf '%s\n' "$@" | awk -v src="$_file" '
    BEGIN {
        fn = ""; pend = ""; depth = 0; ln = 0; incomment = 0
        while ((getline raw < src) > 0) {
            ln++
            l = raw
            # Strip what would otherwise put stray braces into the count:
            # block comments (including multi-line), line comments, string and
            # character literals. Crude on purpose -- see the safe-direction
            # note above: over-stripping withholds attribution, it never
            # invents one.
            if (incomment) {
                if (index(l, "*/")) { l = substr(l, index(l, "*/") + 2); incomment = 0 }
                else { l = "" }
            }
            while (index(l, "/*")) {
                pre = substr(l, 1, index(l, "/*") - 1)
                rest = substr(l, index(l, "/*") + 2)
                if (index(rest, "*/")) { l = pre " " substr(rest, index(rest, "*/") + 2) }
                else { l = pre; incomment = 1; break }
            }
            sub(/\/\/.*$/, "", l)
            gsub(/"[^"]*"/, "\"\"", l)
            gsub(/'"'"'[^'"'"']*'"'"'/, "0", l)

            prevdepth = depth
            o = gsub(/\{/, "{", l)
            c = gsub(/\}/, "}", l)

            if (prevdepth == 0) {
                if (l ~ /^[A-Za-z_]/ && index(l, "(") > 0 && l !~ /^[ \t]*#/) {
                    head = substr(l, 1, index(l, "(") - 1)
                    sub(/[ \t]+$/, "", head)
                    if (match(head, /[A-Za-z_][A-Za-z0-9_]*$/))
                        pend = substr(head, RSTART, RLENGTH)
                }
                # A statement that TERMINATES at file scope was a prototype, a
                # declaration or an initialiser -- never a definition header --
                # so it must not leave a name armed for the next brace.
                if (l ~ /;[ \t]*$/) pend = ""
                if (o > 0 && pend != "") { fn = pend; pend = "" }
                else if (o > 0) fn = ""
            }

            depth = prevdepth + o - c
            if (fn != "" && (prevdepth > 0 || o > 0)) owner[ln] = fn
            else owner[ln] = "(file-scope)"
            if (depth <= 0) { depth = 0; fn = "" }
        }
        close(src)
    }
    { print ($1 in owner) ? owner[$1] : "(file-scope)" }'
}

# ---------------------------------------------------------------------------
# cmd_sites [<defect>...]
#
# THE SITE HALF. Runs the REAL injection against a throwaway copy of the tree
# and reports what it actually changed.
#
# WHY A REAL APPLY AND NOT A PARSE OF THE sed EXPRESSIONS: parsing them would
# be a static proxy for what the injection does, and this whole file exists
# because static proxies are purchasable. `facility_defects.sh apply` is the
# same code path inject_and_run.sh runs inside the container, byte for byte,
# and it already refuses (BROKEN FIXTURE) when an anchor does not land.
#
# Emits, TAB separated:
#   SITE <defect> <file-relative-to-src-root> <line> <function>
# ---------------------------------------------------------------------------
# RUN ENTIRELY IN A SUBSHELL, and every local prefixed `_si_`. Both of those
# are deliberate and both were EARNED, not stylistic:
#
#   - the first version set `trap ... EXIT` and then `trap -` to clear it. Its
#     CALLER (cmd_selftest) had its own EXIT trap and its own $_tmp; the
#     callee cleared the caller's trap and deleted the caller's scratch out
#     from under it, and the selftest then reported PASSED over four broken
#     checks. facility_defects.sh's own selftest carries a comment about the
#     identical collision ("the function lost its own $_root to cmd_apply's").
#   - a subshell scopes the trap, so a Ctrl-C still cleans up but nothing
#     leaks into a caller.
cmd_sites() (
    [ -f "$FA_MANIFEST" ] || fa_die "no manifest at $FA_MANIFEST"
    if [ $# -gt 0 ]; then _si_dl="$*"; else _si_dl=$(sh "$FA_MANIFEST" list); fi

    _tmp=$(mktemp -d) || fa_die "mktemp failed"
    trap 'rm -rf "$_tmp"' EXIT INT TERM
    _dl="$_si_dl"

    for _d in $_dl; do
        case "$_d" in *[!A-Za-z0-9_-]*) fa_die "bad defect name: $_d" ;; esac
        _tg=$(sh "$FA_MANIFEST" field "$_d" targets 2>/dev/null)
        [ -n "$_tg" ] || { echo "SITE${TAB}$_d${TAB}(no-targets)${TAB}0${TAB}(unknown)"; continue; }

        rm -rf "$_tmp/tree"
        mkdir -p "$_tmp/tree"
        for _t in $_tg; do
            [ -f "$FA_REPO_ROOT/src/$_t" ] || continue
            mkdir -p "$_tmp/tree/$(dirname "$_t")"
            cp "$FA_REPO_ROOT/src/$_t" "$_tmp/tree/$_t"
            cp "$FA_REPO_ROOT/src/$_t" "$_tmp/pristine-$(echo "$_t" | tr / _)"
        done

        if ! sh "$FA_MANIFEST" apply "$_d" "$_tmp/tree" >"$_tmp/applylog" 2>&1; then
            # A BROKEN FIXTURE here is the manifest's own loud failure, not a
            # verdict about attribution. Surfaced as a row so a consumer sees
            # it rather than silently reading zero sites.
            echo "SITE${TAB}$_d${TAB}(BROKEN-FIXTURE)${TAB}0${TAB}(unknown)"
            continue
        fi

        for _t in $_tg; do
            _new="$_tmp/tree/$_t"
            _old="$_tmp/pristine-$(echo "$_t" | tr / _)"
            if [ ! -f "$_new" ] || [ ! -f "$_old" ]; then continue; fi
            # The changed lines, in PRISTINE line numbering -- the numbering
            # the enclosing-function scan of the pristine file understands.
            _lines=$(diff "$_old" "$_new" \
                     | sed -n 's/^\([0-9]*\)\(,[0-9]*\)\{0,1\}[acd].*$/\1/p' \
                     | sort -un)
            [ -n "$_lines" ] || continue
            # shellcheck disable=SC2086
            _fns=$(enclosing_functions "$_old" $_lines)
            printf '%s\n' "$_lines" > "$_tmp/ln"
            printf '%s\n' "$_fns"   > "$_tmp/fn"
            paste "$_tmp/ln" "$_tmp/fn" | while IFS="$TAB" read -r _l _f; do
                printf 'SITE\t%s\t%s\t%s\t%s\n' "$_d" "$_t" "$_l" "$_f"
            done
        done
    done
)

# ---------------------------------------------------------------------------
# cmd_attribute [<defect>...]
#
# THE JOIN. SITE rows (what was really mutated) x RED rows (what really changed
# verdict). Only RUN rows whose verdict is `pass` contribute: a control the
# driver itself rejected observed a red set the driver rejected, and the record
# header says readers must exclude those.
#
# Emits, TAB separated:
#   ATTR <suite> <assertion> <file> <function> <defect>
# ---------------------------------------------------------------------------
# FA_ATTR_CACHE: a caller that asks many `depends` questions about ONE tree can
# name a file here and the join is computed once. Correctness does not depend on
# it -- an absent or empty cache is recomputed -- but the register asks one
# question per OVMX-EXECUTIVE claim, and without this each one re-ran all 42
# injections, taking the gate from seconds to minutes. The cache is keyed on
# nothing and lives in the caller's own scratch: a stale cache is the caller's
# bug to avoid by using a fresh path per run, which is why this is opt-in rather
# than a default temp file.
cmd_attribute() {
    [ -f "$FA_RECORD" ] || fa_die "no execution record at $FA_RECORD -- nothing has been measured"
    if [ $# -eq 0 ] && [ -n "${FA_ATTR_CACHE:-}" ] && [ -s "${FA_ATTR_CACHE}" ]; then
        cat "$FA_ATTR_CACHE"
        return 0
    fi
    if [ $# -eq 0 ] && [ -n "${FA_ATTR_CACHE:-}" ]; then
        _ac="$FA_ATTR_CACHE"
        FA_ATTR_CACHE='' cmd_attribute > "$_ac"
        cat "$_ac"
        return 0
    fi
    _sites=$(cmd_sites "$@")
    printf '%s\n' "$_sites" | awk -v rec="$FA_RECORD" -F'\t' '
    $1 == "SITE" && $3 !~ /^\(/ && $5 !~ /^\(/ {
        key = $2
        site[key] = site[key] SUBSEP $3 "\t" $5
        n[key]++
    }
    END {
        while ((getline l < rec) > 0) {
            if (l ~ /^#/ || l == "") continue
            split(l, r, "\t")
            if (r[1] == "RUN" && r[4] == "pass") ok[r[2]] = 1
            if (r[1] == "RED") { nred++; rd[nred] = l }
        }
        close(rec)
        for (i = 1; i <= nred; i++) {
            split(rd[i], r, "\t")
            d = r[2]; suite = r[3]; assertion = r[4]
            if (!(d in ok)) continue
            if (!(d in site)) continue
            k = split(site[d], parts, SUBSEP)
            for (j = 1; j <= k; j++) {
                if (parts[j] == "") continue
                split(parts[j], fp, "\t")
                key = suite SUBSEP assertion SUBSEP fp[1] SUBSEP fp[2] SUBSEP d
                if (key in seen) continue
                seen[key] = 1
                printf "ATTR\t%s\t%s\t%s\t%s\t%s\n", suite, assertion, fp[1], fp[2], d
            }
        }
    }' | sort -u
}

# ---------------------------------------------------------------------------
# cmd_depends <suite> <function>
#
# THE GATE QUERY. rc=0 iff SOME assertion in <suite> was OBSERVED to change
# verdict when <function> was mutated.
#
# <suite> is a suite name as the harness banners it (test_syssvc_ef_mproc), or
# a path -- the basename minus .c is taken, so a register `proof=` field can be
# passed straight through.
# ---------------------------------------------------------------------------
cmd_depends() {
    [ $# -eq 2 ] || fa_die "usage: depends <suite> <function>"
    _suite=$(basename "$1"); _suite=${_suite%.c}
    _fn="$2"
    cmd_attribute | awk -F'\t' -v s="$_suite" -v f="$_fn" \
        '$2 == s && $5 == f { found = 1; print } END { exit(found ? 0 : 1) }'
}

cmd_functions() {
    cmd_attribute | cut -f4,5 | sort -u
}

# ---------------------------------------------------------------------------
# cmd_handlers <src-root>
#
# vms-2b2, ANSWERED BY EXECUTION INSTEAD OF BY LINE OVERLAP.
#
# That item measured "9 of 33 vms_ioctl_* handlers sit inside a mutation hunk"
# and said in terms that the measure is LINE-LEVEL, not behavioural: a defect
# in shared eflag code reddens the eflag handlers without the hunk sitting
# literally inside vms_ioctl_setef. The honest question it names is
#
#     "which wired handlers have NO mutation that changes what they return"
#
# and this prints exactly that, in three columns that are NOT the same thing:
#
#   MEASURED   some assertion's verdict was OBSERVED to change when this
#              handler (or a function the injection landed in) was mutated.
#   PROBED     a defect's injection lands in this handler, but no observed
#              RED is attributed to it -- the probe exists and fired nothing.
#   UNPROBED   nothing in the manifest mutates this handler at all. NOT
#              "untested": nothing measured it. See `caveats`.
# ---------------------------------------------------------------------------
cmd_handlers() {
    _root="${1:-$FA_REPO_ROOT/src}"
    [ -d "$_root/kernel" ] || fa_die "no $_root/kernel"
    _attr=$(cmd_attribute)
    _sites=$(cmd_sites)
    grep -hoE '^(long|int|static long|static int)[ \t]+vms_ioctl_[A-Za-z0-9_]+' "$_root"/kernel/*.c 2>/dev/null \
        | sed 's/.*\(vms_ioctl_[A-Za-z0-9_]*\)/\1/' | sort -u \
        | while read -r _h; do
            [ -n "$_h" ] || continue
            # NOTE: the flag, not `exit 0` in the rule. awk's `exit` from a rule
            # still runs END, and an END that exits 1 overrides it -- which is
            # exactly how the first version of this loop reported all 33
            # handlers UNPROBED while `attribute` was printing 254 rows.
            if printf '%s\n' "$_attr" | awk -F'\t' -v h="$_h" 'BEGIN{f=0} $5 == h {f=1} END { exit(f?0:1) }'; then
                _n=$(printf '%s\n' "$_attr" | awk -F'\t' -v h="$_h" '$5 == h' | cut -f2,3 | sort -u | wc -l)
                printf 'MEASURED\t%s\t%s assertion(s)\n' "$_h" "$_n"
            elif printf '%s\n' "$_sites" | awk -F'\t' -v h="$_h" 'BEGIN{f=0} $5 == h {f=1} END { exit(f?0:1) }'; then
                printf 'PROBED\t%s\tinjection lands here, no observed red attributed\n' "$_h"
            else
                printf 'UNPROBED\t%s\tno defect mutates this handler -- UNMEASURED, not untested\n' "$_h"
            fi
        done
}

# Prints the "does not prove" block from this file's own header, so a consumer
# cannot quote a number from `handlers` or `attribute` without the limits that
# come with it. Sourced from the header rather than restated here: two copies of
# a caveat is one copy that goes stale.
cmd_caveats() {
    awk '/^# WHAT THIS DOES NOT PROVE/ { on = 1 }
         on && /^# COMMANDS$/          { exit }
         on                            { sub(/^# ?/, ""); print }' "$0"
}

# ---------------------------------------------------------------------------
# cmd_selftest
#
# NEGATIVE CONTROLS FOR THIS FILE. Every check below states the failure mode it
# exists to catch, and each is a control that CAN fail -- a selftest that only
# ever confirms is the thing this program keeps proving is worthless.
# ---------------------------------------------------------------------------
cmd_selftest() {
    _st_bad=0
    _st_tmp=$(mktemp -d) || fa_die mktemp
    trap 'rm -rf "$_st_tmp"' EXIT INT TERM

    # EVERY LOCAL IN THIS FUNCTION IS `_st_`-PREFIXED. Not style: the first
    # version shared $_tmp with cmd_sites, which deleted it mid-run, and this
    # selftest then printed SELFTEST PASSED over four checks whose inputs did
    # not exist. A selftest that certifies while broken is the exact defect
    # class this file is trying to measure, so the checks below now treat a
    # missing input as a FAILURE and never as a skip.

    echo "--- 1. the enclosing-function scan finds real functions in the real sources ---"
    # Catches: a parser that returns (file-scope) for everything, which would
    # make every claim unpaid and every handler UNPROBED -- a silent all-refuse.
    # ALSO catches its own scratch vanishing, which is how this check lied once.
    cmd_sites > "$_st_tmp/sites" 2>"$_st_tmp/siteerr"
    if [ ! -s "$_st_tmp/sites" ]; then
        echo "  FAIL: cmd_sites produced no output at all (scratch: $_st_tmp)"
        sed 's/^/        | /' "$_st_tmp/siteerr" 2>/dev/null
        _st_bad=1
    else
        _st_tot=$(grep -c '^SITE' "$_st_tmp/sites" || true)
        _st_named=$(awk -F'\t' '$1=="SITE" && $5 !~ /^\(/' "$_st_tmp/sites" | wc -l)
        echo "    $_st_named of $_st_tot site rows name an enclosing function"
        if [ "${_st_tot:-0}" -lt 40 ]; then
            echo "  FAIL: only $_st_tot site rows -- the injection is not being applied"; _st_bad=1
        elif [ "$_st_named" -lt $(( _st_tot / 2 )) ]; then
            echo "  FAIL: fewer than half the sites resolved to a function ($_st_named/$_st_tot)"; _st_bad=1
        else
            echo "  ok: the scan resolves the majority of sites to a named function"
        fi
    fi

    echo "--- 2. a known site lands in the function the source really defines ---"
    # Catches: an off-by-N in the diff-to-line mapping. eflag-clref-noop's sed
    # replaces the bit-clear; whatever function that is, it must be a real
    # definition in the file the row names. An EMPTY result is a FAILURE, not a
    # silent pass -- a `while read` over nothing exits 0.
    : > "$_st_tmp/chk2"
    awk -F'\t' '$1=="SITE" && $2=="eflag-clref-noop"' "$_st_tmp/sites" 2>/dev/null > "$_st_tmp/site2"
    if [ ! -s "$_st_tmp/site2" ]; then
        echo "  FAIL: no SITE row for eflag-clref-noop -- check 2 has nothing to judge"
        _st_bad=1
    else
        while IFS="$TAB" read -r _st_tag _st_d _st_f _st_l _st_fn; do
            [ "$_st_tag" = "SITE" ] || continue
            if [ "$_st_fn" = "(file-scope)" ]; then
                echo "  FAIL: eflag-clref-noop resolved to file scope"; echo bad >> "$_st_tmp/chk2"; continue
            fi
            if grep -qE "(^|[^A-Za-z0-9_])${_st_fn}[ \t]*\(" "$FA_REPO_ROOT/src/$_st_f" 2>/dev/null; then
                echo "  ok: $_st_d -> $_st_f:$_st_l in $_st_fn(), which $_st_f really defines"
            else
                echo "  FAIL: $_st_fn is not defined in $_st_f"; echo bad >> "$_st_tmp/chk2"
            fi
        done < "$_st_tmp/site2"
        [ -s "$_st_tmp/chk2" ] && _st_bad=1
    fi

    echo "--- 3. ATTR never names a function no defect's SITE really mutates (the recorded trap) ---"
    # THE CONTROL THIS FILE EXISTS FOR (vms-38c), REWORKED (vms-a4d, overturning
    # the vms-38c rework that shipped a tautology).
    #
    # THE BUG THAT WAS HERE: the previous version picked its anchor as the
    # FIRST handler `cmd_handlers` reported UNPROBED -- which cmd_handlers
    # defines (see above) as "no ATTR row AND no SITE row names it" -- and then
    # asserted cmd_attribute had NO ROW NAMING THAT SAME HANDLER. That is the
    # negation of the exact predicate the anchor was selected by: it could not
    # fail for any state of the instrument, because whichever handler cleared
    # the "no ATTR row" test to become the anchor was, by that same test,
    # already known to clear it. Proved by mutation: poisoning FA_ATTR_CACHE
    # with a fabricated `ATTR test_syssvc_ef_mproc / BOGUS ASSERTION /
    # kernel/vms_dev.c / vms_ioctl_getlki / bogus-defect` row left this check
    # printing "ok" for vms_ioctl_alloc while vms_ioctl_getlki -- a DIFFERENT,
    # genuinely UNPROBED handler -- silently carried the fabrication. The check
    # re-anchored around the fabrication instead of catching it.
    #
    # THE FIX: stop picking ONE anchor from ATTR's own complement. Assert the
    # join's actual invariant, over EVERY row at once: cmd_attribute's function
    # column can never contain a function that cmd_sites' function column does
    # not ALSO contain, because that is what the join in cmd_attribute is
    # DEFINED to produce (see the awk above: `if (!(d in site)) continue`).
    # Checked as a set difference, a fabrication naming ANY function --
    # not one hand-picked in advance -- is caught. And this check is proven
    # CAPABLE of failing, in this same run, by replaying the auditor's exact
    # fabrication against a scratch cache and confirming THIS SAME LOGIC flags
    # it as orphaned.
    cmd_sites > "$_st_tmp/sites3" 2>/dev/null
    awk -F'\t' '$1=="SITE" && $5 !~ /^\(/ {print $5}' "$_st_tmp/sites3" | sort -u > "$_st_tmp/site_fns3"
    FA_ATTR_CACHE='' cmd_attribute 2>/dev/null | cut -f5 | sort -u > "$_st_tmp/attr_fns3"
    _st_orphan=$(comm -23 "$_st_tmp/attr_fns3" "$_st_tmp/site_fns3" 2>/dev/null | grep -v '^$')
    if [ -n "$_st_orphan" ]; then
        echo "  FAIL: cmd_attribute names a function no defect's SITE row mutates:"
        printf '%s\n' "$_st_orphan" | sed 's/^/        | /'
        _st_bad=1
    else
        echo "  ok: every function cmd_attribute names is a function some defect's SITE"
        echo "      row really mutates -- an ignored call, a stale cache, or a hand-edit"
        echo "      cannot manufacture an ATTR row outside that join"
    fi

    echo "    proving check 3 CAN fail: replaying the auditor's fabricated-cache attack"
    _st_fake3="$_st_tmp/fake_attr_cache3"
    printf 'ATTR\ttest_syssvc_ef_mproc\tBOGUS ASSERTION\tkernel/vms_dev.c\tvms_ioctl_getlki\tbogus-defect\n' > "$_st_fake3"
    FA_ATTR_CACHE="$_st_fake3" cmd_attribute 2>/dev/null | cut -f5 | sort -u > "$_st_tmp/attr_fns3_fake"
    _st_orphan_fake=$(comm -23 "$_st_tmp/attr_fns3_fake" "$_st_tmp/site_fns3" 2>/dev/null | grep -v '^$')
    if [ -z "$_st_orphan_fake" ]; then
        echo "  FAIL: FALSIFIABILITY PROOF FAILED -- the fabricated row (vms_ioctl_getlki,"
        echo "        which has no real SITE) was NOT flagged as orphaned. This check is a"
        echo "        tautology again; the 'ok' above proves nothing."
        _st_bad=1
    else
        echo "  ok: the fabricated row IS flagged as orphaned ($_st_orphan_fake) -- this"
        echo "      check can fail, and the pristine 'ok' above is therefore real evidence"
    fi

    echo "--- 4. the instrument is not vacuous: a real dependency IS attributed ---"
    # Catches the opposite drift -- a `depends` that always says no would pass
    # check 3 trivially and gate nothing.
    cmd_attribute > "$_st_tmp/attr" 2>/dev/null
    if [ -s "$_st_tmp/attr" ]; then
        echo "  ok: $(wc -l < "$_st_tmp/attr") attribution row(s) -- the join is live"
    else
        echo "  FAIL: zero attribution rows; 'depends' would refuse everything"; _st_bad=1
    fi

    echo "--- 5. a FABRICATED record row cannot invent attribution for an unprobed function ---"
    # Catches: a consumer reading the record as the whole truth. A hand-added
    # RED row still needs a SITE row for the same defect, and SITE rows are
    # re-derived from a real injection, so a red attributed to a function no
    # sed touches is unreachable by editing the record alone.
    #
    # The check is only meaningful if the fabricated row PRODUCED something --
    # an empty result would pass the "does not reach wflor" test for free, and
    # that is how check 5 lied when its scratch had been deleted.
    _st_real_fn=$(awk -F'\t' '$1=="SITE" && $2=="eflag-clref-noop" && $5 !~ /^\(/ {print $5; exit}' "$_st_tmp/sites3" 2>/dev/null)
    if ! cp "$FA_RECORD" "$_st_tmp/rec" 2>/dev/null; then
        echo "  FAIL: could not copy the record from $FA_RECORD"; _st_bad=1
    elif [ -z "$_st_real_fn" ]; then
        echo "  FAIL: eflag-clref-noop has no real SITE row to check against"; _st_bad=1
    else
        printf 'RED\teflag-clref-noop\ttest_syssvc_ef_mproc\tFABRICATED ASSERTION\n' >> "$_st_tmp/rec"
        FA_RECORD="$_st_tmp/rec" cmd_attribute 2>/dev/null \
            | awk -F'\t' '$3 == "FABRICATED ASSERTION"' > "$_st_tmp/fab"
        cut -f5 "$_st_tmp/fab" | sort -u > "$_st_tmp/fab_fns"
        _st_f1=$(tr '\n' ' ' < "$_st_tmp/fab_fns")
        echo "    a fabricated RED row attributes only to: ${_st_f1:-(nothing)} (real SITE: $_st_real_fn)"
        if [ ! -s "$_st_tmp/fab" ]; then
            echo "  FAIL: the fabricated row produced NO rows at all, so this check judged"
            echo "        nothing. It must attribute to eflag-clref-noop's real site."
            _st_bad=1
        elif [ "$(wc -l < "$_st_tmp/fab_fns")" -ne 1 ] || [ "$(cat "$_st_tmp/fab_fns")" != "$_st_real_fn" ]; then
            echo "  FAIL: attributed to '$_st_f1', not exactly eflag-clref-noop's real site"
            _st_bad=1
        else
            echo "  ok: it is confined to the function that defect's sed REALLY mutates --"
            echo "      the record can lie about WHICH ASSERTION, never about WHICH FUNCTION"
        fi
    fi

    echo "--- 6. a defect the driver REJECTED contributes no attribution ---"
    # Catches an UNTESTED BRANCH, found by looking rather than by it failing:
    # cmd_attribute only admits RED rows whose defect has a RUN row with verdict
    # `pass`, because the record's own header says a control the driver rejected
    # observed a red set the driver rejected. All 42 RUN rows in the committed
    # record are `pass`, so that filter never fires in normal use and would sit
    # unexercised -- a line of gate logic nobody has ever seen work. Here the
    # verdict is flipped to `fail` in a scratch copy and the rows must vanish.
    if ! cp "$FA_RECORD" "$_st_tmp/rec6" 2>/dev/null; then
        echo "  FAIL: could not copy the record"; _st_bad=1
    else
        _st_n0=$(FA_ATTR_CACHE='' FA_RECORD="$_st_tmp/rec6" cmd_attribute 2>/dev/null \
                 | awk -F'\t' '$6 == "eflag-clref-noop"' | wc -l)
        sed 's/^RUN\(\t\)eflag-clref-noop\(\t.*\t\)pass$/RUN\1eflag-clref-noop\2fail/' \
            "$_st_tmp/rec6" > "$_st_tmp/rec6f"
        if cmp -s "$_st_tmp/rec6" "$_st_tmp/rec6f"; then
            echo "  FAIL: the verdict flip did not land -- the RUN row shape changed, so this"
            echo "        check judged nothing. Re-anchor it."
            _st_bad=1
        else
            _st_n1=$(FA_ATTR_CACHE='' FA_RECORD="$_st_tmp/rec6f" cmd_attribute 2>/dev/null \
                     | awk -F'\t' '$6 == "eflag-clref-noop"' | wc -l)
            echo "    eflag-clref-noop attribution rows: verdict=pass -> $_st_n0, verdict=fail -> $_st_n1"
            if [ "$_st_n0" -eq 0 ]; then
                echo "  FAIL: zero rows even at verdict=pass -- nothing to withdraw"; _st_bad=1
            elif [ "$_st_n1" -ne 0 ]; then
                echo "  FAIL: a rejected control still contributed $_st_n1 attribution row(s)"; _st_bad=1
            else
                echo "  ok: all $_st_n0 row(s) withdrawn when the driver's verdict is 'fail'"
            fi
        fi
    fi

    rm -rf "$_st_tmp"
    trap - EXIT INT TERM
    if [ "$_st_bad" -ne 0 ]; then
        echo "SELFTEST FAILED"
        return 1
    fi
    echo "SELFTEST PASSED"
    return 0
}

case "${1:-}" in
    sites)      shift; cmd_sites "$@" ;;
    attribute)  shift; cmd_attribute "$@" ;;
    depends)    shift; cmd_depends "$@" ;;
    functions)  shift; cmd_functions "$@" ;;
    handlers)   shift; cmd_handlers "$@" ;;
    caveats)    cmd_caveats ;;
    selftest)   cmd_selftest ;;
    *) echo "usage: $0 {sites|attribute|depends|functions|handlers|caveats|selftest}" >&2; exit 2 ;;
esac
