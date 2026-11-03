#!/bin/bash
# TEST: DIFFERENCES lists, counts and returns its status as VAX V7.3 did
# EXPECT: contains:DIFF_ORACLE_MATCH
# EXPECT_NOT: contains:DIFF_ORACLE_DIFF
# TIMEOUT: 40
#
# The SAME procedure the real VAX ran (tools/oracle/semantic/comgen.py over
# specs-dcl/diff.py) is run by OVMX's DCL here. Each case prints the listing
# DIFFERENCES/OUTPUT wrote, one record per line (<CASE>.Lnn), its record count
# (<CASE>.N) and the $STATUS (<CASE>), and every golden line of
# docs/oracle/semantics/diff/vax73.txt must appear verbatim -- except a line
# listed in docs/oracle/semantics/known-diff.d/diff-* (the semantic-oracle
# ratchet), which must still DIFFER: a listed line that now matches fails too,
# so the list only shrinks. The booted semantic-oracle gate
# (tests/qemu/test_semantic_oracle.sh) checks the same family on the real
# executive; this is the fast host-side twin.
set -u
VMSDCL="${VMSDCL:-vmsdcl}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GOLD="$ROOT/docs/oracle/semantics/diff/vax73.txt"
TDIR="dcl_semdiff_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"
# The procedure works in SYS$SYSDEVICE:[SPDIFF], a directory every host ctest on
# this machine shares -- a leftover or concurrent one bumps SP_D.LIS to ;2
# (vms-f63). Give THIS run a private work directory of the same width (so the
# listing wraps identically): reserve a free SPxxxx with an atomic mkdir, run
# the procedure in it, map its name back to SPDIFF to compare with the golden,
# and remove it on every exit.
SPD=""
while [ -z "$SPD" ]; do
    cand="SP$(od -An -N3 -tx1 /dev/urandom | tr -d ' \n' | tr a-f A-F | cut -c1-4)"
    mkdir "/vms/$cand" 2>/dev/null && SPD="$cand"
done
trap 'rm -rf "/vms/$TDIR" "/vms/$SPD"' EXIT
python3 "$ROOT/tools/oracle/semantic/comgen.py" "$ROOT/tools/oracle/semantic/specs-dcl/diff.py" |
    sed "s/SPDIFF/$SPD/g" > "/vms/$TDIR/sp_diff.com"
got=$({ echo "SET DEFAULT SYS\$SYSDEVICE:[$VDIR]"; echo "@SP_DIFF.COM"; } | "$VMSDCL" 2>&1 |
      tr -d '\r' | awk '/^=== SEMPROBE diff BEGIN ===$/{on=1} on{print} /^=== SEMPROBE diff END ===$/{exit}' |
      sed "s/$SPD/SPDIFF/g")
rm -rf "/vms/$TDIR" "/vms/$SPD"
# compare in one pass: every golden line verbatim unless known-diff (then it
# must still differ), and no DIFF.* line the VAX did not print
printf '%s\n' "$got" | python3 -c '
import glob, sys
gold, kdir = sys.argv[1], sys.argv[2]
want = [l.rstrip("\n") for l in open(gold) if not l.startswith("#")]
got = set(l.rstrip("\n") for l in sys.stdin)
gotid = {}
for l in got:
    gotid.setdefault(l.split(" ", 1)[0], l)
known = set()
for f in glob.glob(kdir + "/diff-*"):
    for l in open(f):
        t = l.split("#", 1)[0].split()
        if len(t) == 2 and t[0] == "diff":
            known.add(t[1])
bad = 0
if not any(l.startswith("DIFF.") for l in want):
    print("no golden cases"); bad = 1
for w in want:
    cid = w.split(" ", 1)[0]
    if cid in known:
        if w in got:
            print("STALE known-diff (now matches): " + cid); bad = 1
    elif w not in got:
        print("DIFF: want [%s] got [%s]" % (w, gotid.get(cid, "<none>"))); bad = 1
wids = set(w.split(" ", 1)[0] for w in want)
for l in sorted(got):
    if l.startswith("DIFF.") and l.split(" ", 1)[0] not in wids:
        print("EXTRA: " + l); bad = 1
print("DIFF_ORACLE_DIFF" if bad else "DIFF_ORACLE_MATCH")
' "$GOLD" "$ROOT/docs/oracle/semantics/known-diff.d"
