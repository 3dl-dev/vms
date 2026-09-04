#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# cluster_wire_safety_gate.sh - the PRE-FLIGHT gate that must pass before OVMX
# is allowed to put cluster traffic on a wire a real VMS node can hear.
#
# WHY THIS EXISTS. Twice now we have discovered a way OVMX crashes a real VMS
# peer by CRASHING ONE: `CNXMGRERR` (E76, an unbacked transaction envelope) and
# `INVEXCEPTN` (E78, 254 acks in 31.6 ms answering a 254-frame burst
# frame-for-frame). A production VMScluster cannot be the place we discover the
# third. This gate reads a capture and reports every OVMX-originated frame that
# sits outside the envelope every real VMS node in the reference corpus keeps.
#
# The taxonomy, the invariants and the grounding for every threshold are in
# docs/cluster-crash-safety.md. The checker is
# tools/cluster/cm_wire_safety_audit.py.
#
# USAGE
#   tools/ci/cluster_wire_safety_gate.sh                    self-test only (CI)
#   tools/ci/cluster_wire_safety_gate.sh <pcap> [<pcap>...]  audit captures
#   OVMX_MAC=<mac> tools/ci/cluster_wire_safety_gate.sh <pcap>
#
# WITH NO ARGUMENTS it runs the checker's own self-test: a synthesized clean
# dialogue that must produce zero findings, and one single-factor violation
# fixture per vector that must each be detected. That is the half CI runs on
# every PR -- a gate whose teeth are never exercised proves nothing.
#
# WITH CAPTURES it audits them. Run it on the pcap from the previous lab fire
# BEFORE the next one: a FATAL finding means the last run put a frame on the
# wire no reference node ever emits, and the fix belongs ahead of the re-fire.
#
# Exit 0 = clean. Exit 1 = a FATAL finding (or the self-test lost a tooth).
# Exit 2 = the gate could not run, which is a failure, not a pass.
#
# POSIX sh: no bashisms, so it runs under dash on a minimal CI image.

set -e

REPO=${OVMX_REPO:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
AUDIT="$REPO/tools/cluster/cm_wire_safety_audit.py"

if [ ! -f "$AUDIT" ]; then
    echo "cluster_wire_safety_gate: $AUDIT not found" >&2
    exit 2
fi

PYTHON=${PYTHON:-python3}
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    echo "cluster_wire_safety_gate: $PYTHON not on PATH" >&2
    exit 2
fi

if [ $# -eq 0 ]; then
    echo "== cluster wire-safety gate: self-test (no capture given)"
    "$PYTHON" "$AUDIT" --self-test
    exit $?
fi

echo "== cluster wire-safety gate: auditing $# capture(s)"
if [ -n "$OVMX_MAC" ]; then
    exec "$PYTHON" "$AUDIT" --ovmx-mac "$OVMX_MAC" "$@"
fi
exec "$PYTHON" "$AUDIT" "$@"
