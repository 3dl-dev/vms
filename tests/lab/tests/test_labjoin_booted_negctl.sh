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

# ---------------------------------------------------------------------------
# CAP-DENIED TEETH (leg (e), the anti-fabrication instrument for THIS PR). The
# gate must (a) refuse a run where CAP_NET_RAW was still present in the booted
# node's context -- that is exactly the ambient-cap crutch the 0.6 LARP rode --
# and (b) refuse a run with no cap evidence at all (fail-closed). Below, flip the
# cap evidence and assert lj_cap_denied_verdict / lj_booted_gate_verdict go RED.
#
# Fixtures are real /proc/<pid>/status Cap masks (verified in a root container):
#   a80415fb = pod default set + NET_ADMIN, with CAP_NET_RAW (bit 13) DROPPED.
#   a80435fb = same but WITH CAP_NET_RAW present (the crutch).
# ---------------------------------------------------------------------------
CAP_DENIED='CapInh: 0000000000000000
CapPrm: 00000000a80415fb
CapEff: 00000000a80415fb
CapBnd: 00000000a80415fb
CapAmb: 0000000000000000'
CAP_CRUTCH='CapInh: 0000000000000000
CapPrm: 00000000a80435fb
CapEff: 00000000a80435fb
CapBnd: 00000000a80435fb
CapAmb: 0000000000000000'
CAP_CRUTCH_BND='CapInh: 0000000000000000
CapPrm: 00000000a80415fb
CapEff: 00000000a80415fb
CapBnd: 00000000a80435fb
CapAmb: 0000000000000000'

capmust_fail() {  # <desc> <evidence>
    local d="$1"
    if lj_cap_denied_verdict "$2" >"$TMP/o" 2>&1; then
        echo "  FAIL: $d -- cap verdict PASSED but should have FAILED"; sed 's/^/      /' "$TMP/o"; FAIL=$((FAIL+1))
    else
        echo "  PASS: $d -- cap verdict correctly FAILED"; PASS=$((PASS+1))
    fi
}
echo "--- cap-denied teeth (leg e): the crutch must never pass ---"
# THE core teeth: CAP_NET_RAW present in effective -> the crutch could be back -> RED.
capmust_fail "leg (e): CAP_NET_RAW present in CapEff (the ambient crutch)" "$CAP_CRUTCH"
# Bounding-set net_raw (a descendant could regain it) -> RED.
capmust_fail "leg (e): CAP_NET_RAW present in CapBnd only (regainable)" "$CAP_CRUTCH_BND"
# No evidence recorded at all -> fail-closed.
capmust_fail "leg (e): no cap evidence recorded (fail-closed)" ""
capmust_fail "leg (e): cap evidence with no CapEff/CapBnd mask (unparseable)" "garbage no masks here"
# Positive control: net_raw cleared -> the cap verdict PASSES (not vacuously red).
if lj_cap_denied_verdict "$CAP_DENIED" >"$TMP/o" 2>&1; then
    echo "  PASS: leg (e) positive control -- cap-denied evidence correctly PASSED"; PASS=$((PASS+1))
else
    echo "  FAIL: leg (e) positive control did not PASS"; sed 's/^/      /' "$TMP/o"; FAIL=$((FAIL+1))
fi

echo "--- full gate = join AND cap-denied: crutch fails even a perfect join ---"
# A PERFECT four-leg join but WITH the cap crutch present -> the full gate is RED.
# This is the exact fabrication the PR exists to catch: green membership riding
# the ambient cap. The join legs all pass; leg (e) drags it red.
if lj_booted_gate_verdict OVMXJ0 "$OVMX_JOINED" "$VAX_SEES_OVMX" 3 "$PCAP_GOOD" "$CAP_CRUTCH" >"$TMP/o" 2>&1; then
    echo "  FAIL: full gate PASSED a perfect join that rode the ambient CAP_NET_RAW crutch"; sed 's/^/      /' "$TMP/o"; FAIL=$((FAIL+1))
else
    echo "  PASS: full gate FAILS a perfect join when CAP_NET_RAW was NOT denied (teeth)"; PASS=$((PASS+1))
fi
# And a perfect join WITH the cap properly denied -> the full gate PASSES.
if lj_booted_gate_verdict OVMXJ0 "$OVMX_JOINED" "$VAX_SEES_OVMX" 3 "$PCAP_GOOD" "$CAP_DENIED" >"$TMP/o" 2>&1; then
    echo "  PASS: full gate PASSES a real join with CAP_NET_RAW denied (executive did the I/O)"; PASS=$((PASS+1))
else
    echo "  FAIL: full gate wrongly FAILED a real join with the cap denied"; sed 's/^/      /' "$TMP/o"; FAIL=$((FAIL+1))
fi

echo ""
echo "  RESULTS: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && { echo "  LABJOIN NEGCTL: PASS (verdict has teeth)"; exit 0; }
echo "  LABJOIN NEGCTL: FAIL"; exit 1
