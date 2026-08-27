#!/bin/sh
#
# boundary_audit_report.sh -- REPORTING input for the executive-boundary AUDIT
# tracer (vms-617, Phase A under vms-040). Sourced/called by the anti-cheat
# audit family (test_runtime_target.sh, test_kif_caller_census.sh) to make the
# tracer's findings VISIBLE.
#
# PHASE A IS REPORTING-ONLY. This never changes the caller's pass/fail status:
# a finding is a raw VMS-semantic syscall an activated image issued instead of
# routing through the executive -- surfaced so the GCC-port / corpus drives
# produce trustworthy results, not a green checkmark over a bypass. Failing the
# build on findings is a LATER, deliberate ratchet (once the runtime path is
# audit-clean), not this. The tracer itself (src/imgact/imgact_boundary_audit.c
# + src/boundary_audit) never drops a finding silently: it emits an
# OVMX_BOUNDARY_AUDIT_CAP line when its fixed store overflows, which this report
# surfaces prominently.
#
# The findings log is $OVMX_BOUNDARY_AUDIT_LOG (what IMGACT was told to write),
# or a conventional run-dir default. Absent log => nothing ran under the tracer
# in this environment (the common case for the static gates) => a one-line note,
# still exit 0.
#
# Usage: boundary_audit_report.sh [log_path]

log="${1:-${OVMX_BOUNDARY_AUDIT_LOG:-${OVMX_RUN_DIR:-/tmp}/ovmx-boundary-audit.jsonl}}"

echo "--- Executive-boundary AUDIT findings (vms-617, Phase A: REPORTING ONLY) ---"

if [ ! -f "$log" ]; then
    echo "  (no findings log at $log -- no image ran under OVMX_BOUNDARY_AUDIT here)"
    echo "  NOTE: reporting-only; findings never fail this gate in Phase A."
    exit 0
fi

total=$(grep -c '"syscall":' "$log" 2>/dev/null || echo 0)
if [ "$total" -eq 0 ]; then
    echo "  findings log present but empty ($log) -- audit was clean."
    exit 0
fi

echo "  findings log: $log ($total finding line(s))"
echo "  by syscall:"
# Count occurrences per syscall name, purely for visibility.
sed -n 's/.*"syscall":"\([^"]*\)".*/\1/p' "$log" 2>/dev/null \
    | sort | uniq -c | sort -rn | sed 's/^/    /'

# Surface the tracer's own overflow marker prominently: nothing is silently
# dropped (design + INV-6). A CAP line means the fixed finding store filled.
if grep -q 'OVMX_BOUNDARY_AUDIT_CAP' "$log" 2>/dev/null; then
    echo "  !! CAP REACHED: the tracer's finding store overflowed and dropped findings:"
    grep 'OVMX_BOUNDARY_AUDIT_CAP' "$log" | sed 's/^/       /'
fi

echo "  NOTE: reporting-only; these findings do NOT fail the gate in Phase A"
echo "        (enforcement/ratchet is a later, deliberate step -- vms-48e / vms-040)."
exit 0
