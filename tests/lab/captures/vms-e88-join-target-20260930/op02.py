import os, sys
sys.path.insert(0,'docs/clean-room/tools')
from pcap import frames, is6007
BODY=72
def sysid(l): return l[4] | (l[5] << 8)
for path in sys.argv[1:]:
    try: fr=[(t,p) for (t,p) in frames(path) if is6007(p) and len(p)>=32]
    except Exception: continue
    if not fr: continue
    t0=fr[0][0]; mac2sid={}
    for t,p in fr:
        s=sysid(p[24:30])
        if s: mac2sid.setdefault(bytes(p[6:12]),s)
    out=[]
    for t,p in fr:
        if len(p)<BODY+20 or p[30] not in (0x4b,0x5b): continue
        cat=p[BODY+8]; op=p[BODY+9]
        if (cat,op) in ((1,2),(1,0x12)) or (cat==4 and False):
            s=sysid(p[24:30]); d=mac2sid.get(bytes(p[0:6]),'?')
            out.append("  t=%8.3f cat=%02x op=%02x %s -> %s%s"%(t-t0,cat,op,s,d, (" class=%02x"%p[BODY+17]) if op==0x12 else ""))
    if out:
        print(os.path.basename(path)); print("\n".join(out))
