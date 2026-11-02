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
python3 "$ROOT/tools/oracle/semantic/comgen.py" "$ROOT/tools/oracle/semantic/specs-dcl/diff.py" \
    > "/vms/$TDIR/sp_diff.com"
got=$({ echo "SET DEFAULT SYS\$SYSDEVICE:[$VDIR]"; echo "@SP_DIFF.COM"; } | "$VMSDCL" 2>&1 |
      tr -d '\r' | awk '/^=== SEMPROBE diff BEGIN ===$/{on=1} on{print} /^=== SEMPROBE diff END ===$/{exit}')
rm -rf "/vms/$TDIR"
# the procedure's own work directory, SYS$SYSDEVICE:[SPDIFF]
for d in /vms/SPDIFF /vms/spdiff; do [ -d "$d" ] && rm -rf "$d"; done
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
