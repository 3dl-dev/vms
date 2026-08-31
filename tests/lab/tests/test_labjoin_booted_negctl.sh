#!/bin/bash
# test_labjoin_booted_negctl.sh (rd vms-fa1a, Rule 7 can-fail proof) - the
# acceptance verdict (labjoin_lib.sh lj_verdict) is the anti-fabrication
# instrument for the 0.6 cluster milestone; this proves it has TEETH. For each
# leg of a real join, flip exactly that leg and assert the verdict goes RED. A
# gate that cannot fail proves nothing.
#
# This ALSO documents the EXPECTED-RED shape of the real gate today: pre-vms-5ad
# a booted OVMX never auto-starts SCS, so its SHOW CLUSTER reports NOSUCHDEV --
# case (A) below is exactly that state, and the verdict correctly FAILS it.
#
# Exit 0 = every flipped-leg case correctly FAILED (the verdict has teeth).
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../tools/labjoin_lib.sh
. "$HERE/../tools/labjoin_lib.sh"

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
PCAP_GOOD="$TMP/good.pcap"; printf 'x OVMXJ0 y 0x6007' >"$PCAP_GOOD"
PCAP_EMPTY="$TMP/empty.pcap"; : >"$PCAP_EMPTY"

OVMX_JOINED='View of Cluster from system OVMXJ0
  VAX1 MEMBER
  VAX2 MEMBER'
OVMX_NOSUCHDEV='%SYSTEM-W-NOSUCHDEV, no such device available'
OVMX_NOVAX='View of Cluster from system OVMXJ0
  (no members)'
VAX_SEES_OVMX='View of Cluster from system VAX1
  VAX1 MEMBER
  VAX2 MEMBER
  OVMXJ0 MEMBER'
VAX_NO_OVMX='View of Cluster from system VAX1
  VAX1 MEMBER
  VAX2 MEMBER'

PASS=0; FAIL=0
must_fail() {  # <desc> <node> <ovmx> <vax> <cn> <pcap>
    local d="$1"; shift
    if lj_verdict "$@" >"$TMP/o" 2>&1; then
        echo "  FAIL: $d -- verdict PASSED but should have FAILED"; sed 's/^/      /' "$TMP/o"; FAIL=$((FAIL+1))
    else
        echo "  PASS: $d -- verdict correctly FAILED"; PASS=$((PASS+1))
    fi
}
must_pass() {  # positive control, so the cases above are not vacuously failing
    local d="$1"; shift
    if lj_verdict "$@" >"$TMP/o" 2>&1; then
        echo "  PASS: $d -- verdict correctly PASSED"; PASS=$((PASS+1))
    else
        echo "  FAIL: $d -- positive control did not PASS"; sed 's/^/      /' "$TMP/o"; FAIL=$((FAIL+1))
    fi
}

echo "=== labjoin verdict negctl (vms-fa1a): each flipped leg must go RED ==="
# (A) OVMX side reports NOSUCHDEV (the pre-vms-5ad reality: SCS never started).
must_fail "leg (a): OVMX SHOW CLUSTER = NOSUCHDEV (SCS not up)" \
    OVMXJ0 "$OVMX_NOSUCHDEV" "$VAX_SEES_OVMX" 3 "$PCAP_GOOD"
# (a') OVMX up but sees no VAX member.
must_fail "leg (a'): OVMX executive up but no VAX member" \
    OVMXJ0 "$OVMX_NOVAX" "$VAX_SEES_OVMX" 3 "$PCAP_GOOD"
# (b) the VAX oracle never admitted the OVMX node.
must_fail "leg (b): vax1 SHOW CLUSTER does not list OVMXJ0" \
    OVMXJ0 "$OVMX_JOINED" "$VAX_NO_OVMX" 3 "$PCAP_GOOD"
# (c) cluster never grew.
must_fail "leg (c): CLUSTER_NODES still 2" \
    OVMXJ0 "$OVMX_JOINED" "$VAX_SEES_OVMX" 2 "$PCAP_GOOD"
# (d) nothing on the wire.
must_fail "leg (d): empty join pcap" \
    OVMXJ0 "$OVMX_JOINED" "$VAX_SEES_OVMX" 3 "$PCAP_EMPTY"
# positive control: all four legs present -> PASS.
must_pass "positive control: all four legs present" \
    OVMXJ0 "$OVMX_JOINED" "$VAX_SEES_OVMX" 3 "$PCAP_GOOD"

echo ""
echo "  RESULTS: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && { echo "  LABJOIN NEGCTL: PASS (verdict has teeth)"; exit 0; }
echo "  LABJOIN NEGCTL: FAIL"; exit 1
