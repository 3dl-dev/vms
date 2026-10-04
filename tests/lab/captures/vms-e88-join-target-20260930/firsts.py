import sys
sys.path.insert(0,'/root'); sys.path.insert(0,'docs/clean-room/tools')
from pcap import frames, is6007
def sysid(l): return l[4] | (l[5] << 8)
path=sys.argv[1]; who=int(sys.argv[2]); since=float(sys.argv[3]) if len(sys.argv)>3 else 0
fr=[(t,p) for (t,p) in frames(path) if is6007(p) and len(p)>=32]
t0=fr[0][0]; mac2sid={}
for t,p in fr:
    s=sysid(p[24:30])
    if s: mac2sid.setdefault(bytes(p[6:12]),s)
seen=set()
for t,p in fr:
    if t-t0<since: continue
    s=sysid(p[24:30]); d=mac2sid.get(bytes(p[0:6]))
    if s==who and d and (d,p[30]) not in seen:
        seen.add((d,p[30])); print("t=%8.3f %d -> %d type=%02x"%(t-t0,s,d,p[30]))
    if d==who and s and (('in',s,p[30]) not in seen):
        seen.add(('in',s,p[30])); print("t=%8.3f %d -> %d type=%02x (in)"%(t-t0,s,d,p[30]))
