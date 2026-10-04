"""ADD admissions only: pair each cat-0x01 op-0x02 with the cat-0x01 op-0x12
(class 0x02) it triggers, and ask what distinguishes the member that drove it
from the members that did nothing.  rd vms-1ac."""
import os, sys
sys.path.insert(0,'/home/baron/projects/vms/docs/clean-room/tools')
from pcap import frames, is6007
BODY=72
def sysid(l): return l[4] | (l[5] << 8)
rows=[]
for path in sorted(sys.argv[1:]):
    try: fr=[(t,p) for (t,p) in frames(path) if is6007(p) and len(p)>=32]
    except Exception: continue
    if not fr: continue
    t0=fr[0][0]
    reqs=[]; relays=[]
    seen_by_time=[]
    for t,p in fr:
        s=sysid(p[24:30])
        if s: seen_by_time.append((t,s))
        if len(p)<BODY+20 or p[30] not in (0x4b,0x5b): continue
        if p[BODY+8]!=0x01: continue
        op=p[BODY+9]
        if op==0x02: reqs.append((t,s))
        elif op==0x12 and p[BODY+17]==0x02: relays.append((t,s))
    for (tr,j) in reqs:
        cands=[(t,s) for (t,s) in relays if 0 <= t-tr <= 2.0 and s!=j]
        if not cands: continue
        c=cands[0][1]
        # membership as of the request: everything that transmitted before it, minus the joiner
        members=sorted({s for (t,s) in seen_by_time if t<=tr} - {j})
        if not members: continue
        rows.append((os.path.basename(path), tr-t0, j, c, members))
seen=set(); n=0; hi=0
for (f,t,j,c,m) in rows:
    k=(f,c,tuple(m),j)
    if k in seen: continue
    seen.add(k); n+=1
    top=max(m); ok = (c==top); hi += 1 if ok else 0
    print("%-46s t=%7.2f joiner=%-5d COORD=%-5d members=%-24s highest=%-5d %s" % (
        f,t,j,c,m,top,"HIGHEST" if ok else "*** NOT ***"))
print("\nADD admissions with an observed relay: %d ; coordinator was the HIGHEST-SCSSYSTEMID member: %d" % (n,hi))
