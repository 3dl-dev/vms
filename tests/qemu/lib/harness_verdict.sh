# harness_verdict.sh - SOURCEABLE helper (rd vms-b1f). Not a test; nothing
# here runs at source time.
#
# WHAT IT IS FOR. tests/qemu/run_tests.sh boots QEMU, captures the whole
# transcript, and then has to answer ONE question: did the run report zero
# failed suites? That question was answered by
#
#     if echo "$OUTPUT" | grep -qE "FINAL RESULTS:.*[^0-9]0 suites failed"
#
# under `set -euo pipefail`, and THAT PIPELINE IS THE BUG.
#
# THE MECHANISM, isolated rather than described. `grep -q` exits the instant it
# matches, WITHOUT draining the rest of its input. When the transcript carries
# more than one pipe buffer (~64 KiB) of output AFTER the matching line, `echo`
# is still writing when `grep` goes away, so `echo` takes SIGPIPE and exits
# 141. Under `pipefail` the pipeline's status is the rightmost non-zero one --
# 141 -- so the `if` takes the ELSE branch and the harness prints
# "KERNEL MODULE TESTS FAILED" and exits 1 ON A RUN WITH ZERO FAILURES.
#
# MEASURED, deterministically and with no QEMU involved (the three lines below
# were run as one script; only `set -o pipefail` differs):
#
#     OUTPUT="FINAL RESULTS: 5 suites run, 0 suites failed
#     $(head -c 200000 /dev/zero | tr '\0' 'x')"
#
#     with pipefail:        status=141      <- SIGPIPE. Verdict inverted.
#     without pipefail:     status=0
#     here-string+pipefail: status=0
#
# THE GREP MATCHED IN ALL THREE CASES. Nothing was wrong with the pattern; the
# false verdict was manufactured entirely by the pipeline.
#
# WHY IT WAS FILED AS "HOST-SENSITIVE" AND WHY THAT WORDING IS NOW WRONG. It
# was observed failing on workshop while CI on main exited 0, and was recorded
# as host-sensitive. The mechanism is not sensitive to the host -- it is
# sensitive to HOW MUCH TRAILING OUTPUT FOLLOWS THE MATCH, which varies with
# how many suites a tree has and how chatty they are. A host is only ever a
# proxy for that. Do not go looking for a host difference; look at the
# transcript size after the FINAL RESULTS line.
#
# WHAT IT COST, and this is why it is worth a helper rather than a one-line
# edit in place: `tests/qemu/run_facility_negctl.sh` begins with a pristine
# positive control, and that control runs this harness. On any tree where the
# trailing output crosses a pipe buffer, the positive control "fails", so the
# driver REFUSES TO RUN AT ALL -- and the driver is the only instrument in this
# program that actually executes anything. The refusal itself is correct
# behaviour (rd vms-ecf added it on purpose: a driver whose own control fails
# must not certify). It was refusing for a manufactured reason.
#
# THE FIX IS TO NOT BUILD A PIPELINE. `grep -qE ... "$file"` reads the file
# directly; there is no writer to receive SIGPIPE and no pipeline status for
# pipefail to reinterpret. This helper takes a FILE rather than a string so
# that no caller can reintroduce the shape by piping a variable into it.
#
# WHAT THIS HELPER DOES NOT DO. It reads a transcript. It does not know whether
# the suites were correct, whether they ran against a real /dev/vms, or whether
# the transcript is complete -- a truncated transcript that happens to contain
# the line reads exactly like a whole one. It answers ONE question: does this
# text report zero failed suites. Everything else is the caller's problem, and
# rd vms-2c6 tracks the neighbouring case where a QEMU harness exits nonzero
# while its own accounting reports everything passed.

# The pattern, kept in one place so run_tests.sh and the controls cannot drift
# apart. [^0-9] before the 0 is LOAD-BEARING and predates this helper: without
# it the trailing zero of ANY count ending in zero matches, so "10 suites
# failed" reported ALL KERNEL MODULE TESTS PASSED and exited 0. That was latent
# from the day the harness was written and first fired when the
# executive-absent failure count reached double digits. Do not "simplify" it.
HARNESS_VERDICT_ZERO_RE="FINAL RESULTS:.*[^0-9]0 suites failed"

# harness_verdict_zero_failures <transcript-file>
#
# 0  - the transcript reports zero failed suites
# 1  - it reports a nonzero count, or carries no FINAL RESULTS line at all
# 2  - REFUSAL: the file is missing or unreadable
#
# A transcript with NO FINAL RESULTS line is a FAILURE, not a pass. A run that
# died before printing its own summary has not reported success, and treating
# "the line is absent" as "nothing failed" is the silent-fallback shape
# CLAUDE.md Rule 9 forbids one layer down.
harness_verdict_zero_failures() {
    _hv_file="${1:-}"
    if [ -z "$_hv_file" ] || [ ! -r "$_hv_file" ]; then
        echo "harness_verdict: REFUSING to decide: no readable transcript at '${_hv_file}'" >&2
        return 2
    fi
    # No pipe, by construction. See the header.
    if grep -qE "$HARNESS_VERDICT_ZERO_RE" "$_hv_file"; then
        return 0
    fi
    return 1
}
