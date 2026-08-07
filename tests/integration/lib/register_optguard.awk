# register_optguard.awk (rd vms-ed8 residual #2) -- run once per
# CMakeLists.txt file. Tracks if()/endif() nesting (this tree writes both on
# their own line, never inline with other CMake commands -- verified with
# `grep -rnE 'if\(.*\).*endif\(\)'` over every CMakeLists.txt, zero hits) and,
# for every add_subdirectory() call textually inside a block that this
# configure's OFF options make UNREACHABLE, prints one line naming the file,
# the option, and the (unresolved) path if that path does not resolve under
# src/ or tools/.
#
# INPUTS (via -v):
#   OFFLIST -- space-separated option() names this configure left OFF.
#   RELDIR  -- this CMakeLists.txt's own directory, relative to the source
#              root (used to resolve a relative add_subdirectory() argument).
#
# A block is "OFF-guarded" (does not run in this configure) if its condition,
# read as an AND-chain of terms (this tree uses AND only; OR does not appear
# on any option() name -- verified separately), contains a bare OFF option
# name. `NOT <option>` is handled the other way: that term is TRUE when the
# option is OFF, so it does not, by itself, make the block OFF-guarded (see
# src/vmsdcl/CMakeLists.txt's `if(NOT OVMX_STATIC)`, which RUNS precisely
# because OVMX_STATIC defaults OFF).
BEGIN {
    nopt = split(OFFLIST, offarr, " ")
    for (i = 1; i <= nopt; i++) OFF[offarr[i]] = 1
    depth = 0
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
    cond = $0
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
        # none of the existing add_subdirectory() calls use "..", so no
        # collapsing is implemented; a future one that does would show an
        # unresolved-looking path here rather than a wrong verdict.
        if (resolved !~ /^src\// && resolved !~ /^tools\//) {
            printf "%s:%d: add_subdirectory(%s) resolves to %s, outside src/ and tools/\n", FILENAME, FNR, argpath, resolved
        }
    }
}
