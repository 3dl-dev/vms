#!/bin/bash
# test_lab_identity_unique.sh <repo-root>
#
# vms-1ae -- the cluster-identity uniqueness guard in tests/lab/tools/mk_sysgen.py.
#
# WHAT THIS PROTECTS. SCSNODE and SCSSYSTEMID are cluster-wide unique keys. A
# joiner presenting either one already held by a system the peer knows is
# refused by OpenVMS's configuration poller with
#
#     %PEA0, Remote System Conflicts with Known System - REMOTE NODE <name>
#
# and the join never completes: the VC opens, the peer's connect never arrives,
# CLUSTER_NODES never moves. At the console that is indistinguishable from the
# vms-2f3 stall, so a colliding identity silently converts an experiment into a
# null result filed under the wrong cause. Bracketed live on lab-2 vaxlab-8
# 2026-08-06 (arms 1aeC/1aeE conflict, matched controls 1aeB/1aeD clean).
#
# It is not hypothetical: parallel agents minted OVMXY1 and OVMXP1 both on
# SCSSYSTEMID 1601, and OVMXP2/OVMXY2 both on 1602, because mk_sysgen.py would
# mint whatever it was asked for.
#
# The test is hermetic -- it builds its own SYSG v2 stores in a temp dir and
# never touches the lab volume, k3s, or a VAX. It carries BOTH directions:
# a colliding mint must FAIL and a unique mint must SUCCEED, so the guard
# cannot pass by rejecting everything (or by rejecting nothing).
set -u
ROOT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
MK="$ROOT/tests/lab/tools/mk_sysgen.py"
[ -r "$MK" ] || { echo "FAIL: no mk_sysgen.py at $MK"; exit 1; }

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fails=0
ok(){ echo "  ok   -- $1"; }
no(){ echo "  FAIL -- $1"; fails=$((fails+1)); }

# A minimal but real SYSG v2 store: magic "GSYS", version 2, 64 params, the
# two that matter carrying the right types. Layout from sysgen_params.h, the
# same one mk_sysgen.py patches.
python3 - "$T/sysgen-tmpl.dat" <<'PY'
import struct, sys
HDR, PSZ, N = 12, 148, 64
CUR, TYPE, STRCUR, STRDEF = 32, 129, 130, 138
buf = bytearray(HDR + N * PSZ)
struct.pack_into("<III", buf, 0, 0x53595347, 2, N)
def put(i, name, typ):
    b = HDR + i * PSZ
    buf[b:b+len(name)] = name.encode()
    buf[b+TYPE] = typ
put(0, "SCSNODE", 1)
put(1, "SCSSYSTEMID", 0)
for i in range(2, N):
    put(i, "FILLER%02d" % i, 0)
b = HDR + 0 * PSZ
buf[b+STRCUR:b+STRCUR+8] = b"TMPL\0\0\0\0"
buf[b+STRDEF:b+STRDEF+8] = b"TMPL\0\0\0\0"
struct.pack_into("<I", buf, HDR + 1 * PSZ + CUR, 9999)
open(sys.argv[1], "wb").write(bytes(buf))
PY
TMPL="$T/sysgen-tmpl.dat"
[ -s "$TMPL" ] || { echo "FAIL: fixture template not written"; exit 1; }

mk(){ python3 "$MK" "$@" "$TMPL" >"$T/out" 2>"$T/err"; }

echo "== 1. a first, unique identity mints =="
if mk "$T/sysgen-a.dat" OVMXQ1 4001; then ok "OVMXQ1/4001 minted"
else no "unique mint rejected: $(cat "$T/err")"; fi

echo "== 2. NEGATIVE CONTROL: a fully distinct identity still mints =="
# Without this the guard could pass by rejecting every second mint.
if mk "$T/sysgen-b.dat" OVMXQ2 4002; then ok "OVMXQ2/4002 minted"
else no "second unique mint rejected: $(cat "$T/err")"; fi

echo "== 3. SCSSYSTEMID reuse is refused (lab arm 1aeC) =="
if mk "$T/sysgen-c.dat" OVMXQ3 4001; then
  no "OVMXQ3/4001 minted despite SCSSYSTEMID 4001 already being OVMXQ1's"
else
  grep -q "SCSSYSTEMID 4001 is already" "$T/err" \
    && ok "refused, and the message names the colliding SCSSYSTEMID" \
    || no "refused but the message does not name SCSSYSTEMID: $(cat "$T/err")"
  [ -e "$T/sysgen-c.dat" ] && no "refused but wrote the store anyway"
fi

echo "== 4. SCSNODE reuse is refused (lab arm 1aeE) =="
if mk "$T/sysgen-d.dat" OVMXQ1 4004; then
  no "OVMXQ1/4004 minted despite SCSNODE OVMXQ1 already existing"
else
  grep -q "SCSNODE OVMXQ1 is already" "$T/err" \
    && ok "refused, and the message names the colliding SCSNODE" \
    || no "refused but the message does not name SCSNODE: $(cat "$T/err")"
fi

echo "== 5. the refusal quotes the console line it is preventing =="
mk "$T/sysgen-e.dat" OVMXQ1 4001
grep -q "Remote System Conflicts with Known System" "$T/err" \
  && ok "error text names the %PEA0 consequence" \
  || no "error text does not name the consequence a reader has to recognise"

echo "== 6. re-minting the SAME store path is NOT a collision =="
# A rejoin experiment legitimately remints one identity into its own file.
if mk "$T/sysgen-a.dat" OVMXQ1 4001; then ok "same-path remint allowed"
else no "same-path remint refused -- rejoin tests would be impossible: $(cat "$T/err")"; fi

echo "== 7. VMS truncates SCSNODE to 6, and so does the comparison =="
# OVMXQ1X and OVMXQ1 are the same node on the wire; >6 was already rejected,
# so check the 6-char compare is case-insensitive rather than byte-equality.
if mk "$T/sysgen-g.dat" ovmxq1 4007; then
  no "ovmxq1/4007 minted despite OVMXQ1 existing -- case-only difference"
else ok "case-insensitive SCSNODE comparison"; fi

echo "== 8. --force overrides, and says so =="
if python3 "$MK" --force "$T/sysgen-h.dat" OVMXQ1 4001 "$TMPL" >"$T/out" 2>"$T/err"; then
  [ -e "$T/sysgen-h.dat" ] && ok "--force minted the colliding store" \
                           || no "--force succeeded but wrote nothing"
  grep -q "WARNING" "$T/err" && ok "--force warned" || no "--force was silent"
else no "--force did not override: $(cat "$T/err")"; fi

echo "== 9. --alloc hands out a pair nothing claims =="
PAIR=$(python3 "$MK" --alloc OVMXQ "$T" 2>"$T/err")
read -r ANODE ASYSID <<<"$PAIR"
if [ -z "${ANODE:-}" ] || [ -z "${ASYSID:-}" ]; then
  no "--alloc produced nothing: $(cat "$T/err")"
else
  if mk "$T/sysgen-alloc.dat" "$ANODE" "$ASYSID"; then
    ok "--alloc gave $ANODE/$ASYSID and it mints clean"
  else
    no "--alloc gave $ANODE/$ASYSID but the guard rejects it: $(cat "$T/err")"
  fi
fi

echo "== 10. POSITIVE CONTROL for the detector itself =="
# Prove the guard is reading the stores rather than pattern-matching names:
# with an EMPTY registry it must accept an identity that collides in the
# populated one. The template has to live in the empty dir too (its directory
# is part of the registry, deliberately) and must not itself look like a
# store, or the "empty" registry is not empty.
mkdir -p "$T/empty"; cp "$TMPL" "$T/empty/tmpl.dat"
if python3 "$MK" "$T/empty/sysgen-z.dat" OVMXQ1 4001 "$T/empty/tmpl.dat" \
     >"$T/out" 2>"$T/err"; then
  ok "same identity mints in an empty registry -- the guard reads stores"
else
  no "guard rejects in an EMPTY dir -- it is not reading the registry: $(cat "$T/err")"
fi

echo "== 11. the registry is NOT just the output directory (the 1601 hole) =="
# vms-0fe minted OVMXP1/1601 while vms-AA1's OVMXY1/1601 sat in the lab work
# dir. A guard that only scanned the output directory would let a store
# written to a scratch path through -- which is the bug, not the fix.
mkdir -p "$T/scratch"
if mk "$T/scratch/sysgen-x.dat" OVMXQ9 4001; then
  no "SCSSYSTEMID 4001 minted into a scratch dir -- registry is output-dir only"
else
  grep -q "SCSSYSTEMID 4001 is already" "$T/err" \
    && ok "collision caught across directories via the template's registry" \
    || no "refused for the wrong reason: $(cat "$T/err")"
fi

echo
if [ $fails -eq 0 ]; then echo "PASS: lab identity uniqueness guard"; exit 0; fi
echo "FAIL: $fails check(s) failed"; exit 1
