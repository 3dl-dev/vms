import sys, gzip, os, tempfile
sys.path.insert(0,'/root'); sys.path.insert(0,'docs/clean-room/tools')
from pcap import frames, is6007
BODY=72
def sysid(l): return l[4] | (l[5] << 8)
J=int(sys.argv[1])
for path in sys.argv[2:]:
    fr=[(t,p) for (t,p) in frames(path) if is6007(p) and len(p)>=32]
    t0=fr[0][0]; mac2sid={}
    for t,p in fr:
        s=sysid(p[24:30])
        if s: mac2sid.setdefault(bytes(p[6:12]),s)
    first=None; out=[]
    for t,p in fr:
        s=sysid(p[24:30]); d=mac2sid.get(bytes(p[0:6]))
        if s==J and first is None: first=t
        if p[30] not in (0x4b,0x5b) or len(p)<BODY+20: continue
        cat,op=p[BODY+8],p[BODY+9]
        if cat not in (0x01,0x81,0x04): continue
        if s==J or d==J:
            out.append("   %+8.3f %d->%s cat=%02x op=%02x"%(t-(first or t),s,d,cat,op))
    print(os.path.basename(os.path.dirname(path)) or path)
    print("\n".join(out[:int(os.environ.get('N','40'))]))
