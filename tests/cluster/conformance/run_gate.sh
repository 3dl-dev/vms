#!/bin/bash
# run_gate.sh <build-bin-dir> <frame-type>... -- the frame-conformance gate (rd vms-ci.6).
#
# Drives OVMX's REAL senders under their host tests with OVMX_CONFORMANCE_OUT
# set (tests/cluster/host/conformance_dump.h), then checks every emitted byte
# against tests/cluster/conformance/frame-grounding.tsv
# (tools/cluster/frame_conformance.py). Exit 0 only when every frame type named
# on the command line was emitted and every byte of every frame is grounded.
# A negative control then corrupts one grounded field and requires the checker
# to refuse it, so a checker that passes everything cannot pass this gate.
set -u
BIN=${1:?build bin dir}; shift
[ $# -ge 1 ] || { echo "run_gate.sh: name the frame types the fire will exercise" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
TABLE=$HERE/frame-grounding.tsv
CHECK=$ROOT/tools/cluster/frame_conformance.py
DUMP=$(mktemp)
trap 'rm -f "$DUMP" "$DUMP.neg"' EXIT

for t in test_mscp_cl test_cnxman_join test_scs_fsm; do
	if ! OVMX_CONFORMANCE_OUT=$DUMP "$BIN/$t" > /dev/null 2>&1; then
		echo "run_gate.sh: emitter $t itself failed" >&2
		exit 1
	fi
done

python3 "$CHECK" "$TABLE" "$DUMP" --require "$@" || exit 1

# Negative control: the descriptor Con.ID of the first READ replaced by a value
# no row allows (our own Con.ID, the one that halted two real VAXes).
python3 - "$DUMP" "$DUMP.neg" <<'EOF'
import sys
out=open(sys.argv[2],'w'); done=False
for line in open(sys.argv[1]):
    p=line.split()
    if not done and p[0]=='mscp_cmd' and len(p)==4 and p[3][16:18]=='21':
        h=p[3]; h=h[:48]+'07000200'+h[56:]      # abs 96..99 -> 0x00020007
        line=' '.join(p[:3]+[h])+'\n'; done=True
    out.write(line)
sys.exit(0 if done else 1)
EOF
if python3 "$CHECK" "$TABLE" "$DUMP.neg" > /dev/null; then
	echo "run_gate.sh: NEGATIVE CONTROL FAILED -- a corrupted descriptor passed" >&2
	exit 1
fi
echo "run_gate.sh: negative control refused the corrupted frame, as it must"
exit 0
