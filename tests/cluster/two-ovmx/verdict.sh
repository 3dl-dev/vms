#!/bin/bash
# verdict.sh <out-dir> <nodeA> <nodeB> <pcap-frames>
# Reads the two SCSD run logs and reports how far EACH node climbed the
# NEW->MEMBER ladder, and a single baseline verdict. Pure log/pcap reading --
# an observation oracle, no fabricated membership (INV-6).
set -u
OUT="$1"; A="$2"; B="$3"; FRAMES="${4:-?}"
LA="$OUT/scsd-$A.log"; LB="$OUT/scsd-$B.log"

# Ordered progress ladder: marker -> human label. First column is the grep key
# in the SCSD run log; presence means that node reached that rung.
# Chronological up the NEW->MEMBER path. rung-0 (vms-f3e): HELLO -> directed
# HELLO. rung-VC (vms-d60): 0x41 START -> VC OPEN + start_acked. Then the join
# sequencer (OVMX_JOIN_SEQ) 8-step choreography: SCS$DIRECTORY -> MSCP$DISK ->
# VMS$VAXcluster -> add-member.
ladder=(
  "SCSD-I-HELLOSENT|rung-0: multicast HELLO emitted"
  "SCSD-I-DIRHELLO|rung-0: directed HELLO exchanged (NISCA channel)"
  "SCSD-I-STARTTX|rung-VC: 0x41 START sent"
  "SCSD-I-STARTDONE|rung-VC: START handshake done (start_acked)"
  "SCSD-I-VCOPEN|rung-VC: virtual circuit OPEN"
  "SCSD-I-OWNDIRBOUND|seq step 1: our SCS\$DIRECTORY connect accepted"
  "SCSD-I-MSCPBOUND|seq step 5: our MSCP\$DISK connect accepted"
  "SCSD-I-CONNREQ|VMS\$VAXcluster CONNECT-REQUEST sent (non-seq path)"
  "SCSD-I-CONNRESP|peer CONNECT-REQUEST answered/accepted"
  "SCSD-I-VAXCLMEMBER|VMS\$VAXcluster SYSAP connection OPEN"
  "SCSD-I-CMCONFIG|CM config/topology exchanged"
  "SCSD-I-CLUSTATE|cluster membership state learned"
)

reached() { [ -f "$1" ] && grep -qa -- "$2" "$1"; }

echo "===================== TWO-OVMX SCS BASELINE VERDICT ====================="
echo "run dir : $OUT"
echo "pcap    : $(basename "$OUT")/two-ovmx.pcap  ($FRAMES x 0x6007 frames)"
echo
printf "%-46s  %-6s  %-6s\n" "rung" "$A" "$B"
printf "%-46s  %-6s  %-6s\n" "----------------------------------------------" "------" "------"
topA=""; topB=""
for entry in "${ladder[@]}"; do
  key="${entry%%|*}"; label="${entry#*|}"
  ra="no"; rb="no"
  if reached "$LA" "$key"; then ra="YES"; topA="$label"; fi
  if reached "$LB" "$key"; then rb="YES"; topB="$label"; fi
  printf "%-46s  %-6s  %-6s\n" "$label" "$ra" "$rb"
done
echo
echo "highest rung $A : ${topA:-<none - no HELLO even sent>}"
echo "highest rung $B : ${topB:-<none - no HELLO even sent>}"
echo

# The success oracle: BOTH nodes' VMS$VAXcluster SYSAP connection reached OPEN
# (SCSD-I-VAXCLMEMBER), i.e. each sees the other as a cluster member.
if reached "$LA" "SCSD-I-VAXCLMEMBER" && reached "$LB" "SCSD-I-VAXCLMEMBER"; then
  echo "VERDICT: JOIN COMPLETES -- both OVMX nodes reached VMS\$VAXcluster OPEN."
  echo "         Harness is READY to carry rung-1b's DLM round-trip."
  exit 0
fi

echo "VERDICT: JOIN DID NOT COMPLETE (OVMX<->OVMX join gap)."
echo "         See the per-node highest rung above and the pcap for the frame"
echo "         that was sent but drew no expected response."
# Surface the tail of each log so the stall step is visible in CI output.
for L in "$LA" "$LB"; do
  echo; echo "---- tail $(basename "$L") ----"
  [ -f "$L" ] && tail -n 15 "$L" || echo "(no log)"
done
exit 1
