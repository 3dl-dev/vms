# register_optguard.awk (rd vms-ed8 residual #2) -- run once per
# CMakeLists.txt file. Tracks if()/endif() nesting -- INCLUDING a multi-line
# if(...) condition, joined here by counting parens until they balance
# (src/vmslink/CMakeLists.txt:47-48 is one such case in this tree today: a
# grep for 'if\(.*\).*endif\(\)' finds only the single-line-inline shape and
# says nothing about a condition that spans lines, which is what that file
# actually does -- vms-0c3 rework, prior wording here claimed the grep proved
# the single-line case is the only one) -- and, for every add_subdirectory()
# call textually inside a block that this configure's OFF options make
# UNREACHABLE, prints one line naming the file, the option, and the
# (unresolved) path if that path does not resolve under src/ or tools/.
#
# INPUTS (via -v):
#   OFFLIST -- space-separated option() names this configure left OFF.
#   RELDIR  -- this CMakeLists.txt's own directory, relative to the source
#              root (used to resolve a relative add_subdirectory() argument).
#
# A block is "OFF-guarded" (does not run in this configure) if its condition,
# read as an AND-chain of terms (this tree uses AND only combined with a bare
# option() name -- OR does appear once, in src/vmslink/CMakeLists.txt:48-49,
# but combining two MATCHES terms on a non-option variable, not an
# option() name: `grep -oE '^[[:space:]]*option\([[:space:]]*[A-Za-z0-9_]+'`
# lists every option() name this tree declares, and none of them appears as a
# bare OR-joined term in any if() condition found by
# `grep -nE '^\s*if\s*\(' -A1` -- so an AND-only split of an OR-containing
# term simply fails to decompose it, which is safe: it cannot yield a false
# OFF-guard, only a missed one, and none exists to miss here today), contains
# a bare OFF option name. `NOT <option>` is handled the other way: that term
# is TRUE when the option is OFF, so it does not, by itself, make the block
# OFF-guarded (see src/vmsdcl/CMakeLists.txt's `if(NOT OVMX_STATIC)`, which
# RUNS precisely because OVMX_STATIC defaults OFF).
BEGIN {
    nopt = split(OFFLIST, offarr, " ")
    for (i = 1; i <= nopt; i++) OFF[offarr[i]] = 1
    depth = 0
}
function count_char(s, ch,    n, i) {
    n = 0
    for (i = 1; i <= length(s); i++) if (substr(s, i, 1) == ch) n++
    return n
}
function is_off_guarded(cond,    c, terms, nt, i, term, neg, hit) {
    c = cond
    gsub(/[()]/, " ", c)
    nt = split(c, terms, /[ \t]+AND[ \t]+/)
    hit = 0
    for (i = 1; i <= nt; i++) {
        term = terms[i]
        gsub(/^[ \t]+|[ \t]+$/, "", term)
        neg = 0
        if (term ~ /^NOT[ \t]+/) { neg = 1; sub(/^NOT[ \t]+/, "", term) }
        if (term in OFF && !neg) hit = 1
    }
    return hit
}
/^[ \t]*if[ \t]*\(/ {
    raw = $0
    bal = count_char(raw, "(") - count_char(raw, ")")
    while (bal > 0 && (getline extra) > 0) {
        raw = raw " " extra
        bal += count_char(extra, "(") - count_char(extra, ")")
    }
    cond = raw
    sub(/^[ \t]*if[ \t]*\(/, "", cond); sub(/\)[ \t]*$/, "", cond)
    depth++
    guardflag[depth] = is_off_guarded(cond) || (depth > 1 && guardflag[depth - 1])
    next
}
/^[ \t]*endif[ \t]*\(/ {
    delete guardflag[depth]
    if (depth > 0) depth--
    next
}
{
    if (depth > 0 && guardflag[depth] && $0 ~ /add_subdirectory[ \t]*\(/) {
        line = $0
        sub(/^.*add_subdirectory[ \t]*\(/, "", line)
        sub(/\).*$/, "", line)
        gsub(/^[ \t]+|[ \t]+$/, "", line)
        argpath = line
        resolved = (RELDIR == "." ? argpath : RELDIR "/" argpath)
        # Collapse "a/b/../" segments the cheap way this tree's paths need:
        # `grep -rn add_subdirectory --include=CMakeLists.txt . | grep '\.\.'`
        # is empty today, so no collapsing is implemented; a future
        # add_subdirectory(..." ..") argument would show as an
        # unresolved-looking path here rather than a wrong verdict.
        if (resolved !~ /^src\// && resolved !~ /^tools\//) {
            printf "%s:%d: add_subdirectory(%s) resolves to %s, outside src/ and tools/\n", FILENAME, FNR, argpath, resolved
        }
    }
}
