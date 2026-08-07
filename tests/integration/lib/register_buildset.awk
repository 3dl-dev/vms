# register_buildset.awk (rd vms-ed8) -- parses a cmake-generated
# compile_commands.json into the PRODUCT half of the userspace service
# register's compile set. See tests/integration/test_userspace_service_register.sh
# ("THE PRODUCT COMPILE SET") for the design this implements; this file exists
# on its own, not inline in the gate, so the vms-c19 residual "the parse is BY
# LINE SHAPE and a PARTIAL parse would silently shorten the set" has something
# a fixture can be run against directly, without needing a real cmake to emit
# a broken compile_commands.json (it never has, on any tree measured so far).
#
# INPUTS (via -v):
#   ROOT   -- absolute source root, trailing slash; only files under it count.
#   INSTF  -- path to a newline-separated list of INSTALLED cmake target
#             names (the file may be empty or absent). A target on this list
#             counts as a PRODUCT target no matter which directory declared
#             it -- vms-ed8 residual #1: a target declared under tests/,
#             compiling a tests/ source, that is nevertheless shipped by
#             install(TARGETS ...) left the universe unchanged before this.
#
# OUTPUT: on a clean parse, the deduplicated product-relative source paths,
# one per line, on stdout, and exit 0.
#
# On a PARTIAL parse -- some "file" field was read but its object never
# reached a matching "}" close, so it silently contributed nothing to the set
# instead of erroring (vms-ed8 residual #3) -- prints nothing to stdout,
# prints FAIL: ... to stderr, and exits 1. Measured with the negative control
# in test_userspace_service_register_negctl.sh: a hand-built
# compile_commands.json with one "file" entry whose closing brace is deleted
# used to be read as a compile set of size (n-1) with no complaint; this
# build now refuses instead.
BEGIN {
    total_file = 0; total_closed = 0
    if (INSTF != "") {
        while ((getline iline < INSTF) > 0) {
            if (iline != "") INSTALLED[iline] = 1
        }
        close(INSTF)
    }
}
/^[ \t]*"file"[ \t]*:/ {
    total_file++
    f = $0; sub(/^[^:]*:[ \t]*"/, "", f); sub(/",?[ \t]*$/, "", f)
    curf = f; curt = ""; next
}
/^[ \t]*"output"[ \t]*:/ {
    o = $0; sub(/^[^:]*:[ \t]*"/, "", o); sub(/",?[ \t]*$/, "", o)
    curt = o; next
}
/^[ \t]*\}/ {
    if (curf != "") {
        total_closed++
        if (index(curf, ROOT) == 1) {
            rel = substr(curf, length(ROOT) + 1)
            p = index(curt, ".dir/")
            tgt = (p > 0) ? substr(curt, 1, p + 3) : "?"
            q = index(curt, "/CMakeFiles/")
            tdir = (q > 0) ? substr(curt, 1, q - 1) : ""
            n++; src[n] = rel; tg[n] = tgt
            tname = tgt; sub(/^.*\//, "", tname); sub(/\.dir$/, "", tname)
            if ((rel !~ /^tests\// && tdir !~ /^tests\//) || (tname in INSTALLED)) {
                prodtgt[tgt] = 1
            }
        }
    }
    curf = ""; curt = ""; next
}
END {
    if (total_file != total_closed) {
        dropped = total_file - total_closed
        printf "FAIL: BROKEN BUILD-SET SCAN: compile_commands.json parse is PARTIAL: saw %d \"file\" field(s) but only %d closed object(s) -- %d entr%s never reached a matching object close and would have been silently DROPPED from the compile set.\n", total_file, total_closed, dropped, (dropped == 1 ? "y" : "ies") > "/dev/stderr"
        print "  -> this parse no longer matches what CMake writes, or a translation unit's" > "/dev/stderr"
        print "     entry is shaped differently than the others in this compile_commands.json." > "/dev/stderr"
        print "     Fix the parse; do not let it silently shrink the set the way the directory" > "/dev/stderr"
        print "     glob could (vms-c19)." > "/dev/stderr"
        exit 1
    }
    for (i = 1; i <= n; i++) {
        if (src[i] ~ /^tests\// && !(tg[i] in prodtgt)) continue
        if (src[i] in seen) continue
        seen[src[i]] = 1; print src[i]
    }
}
