import sys
sys.path.insert(0,'/root'); sys.path.insert(0,'docs/clean-room/tools')
from pcap import frames, is6007
BODY=72
def sysid(l): return l[4] | (l[5] << 8)
for path in sys.argv[1:]:
    fr=[(t,p) for (t,p) in frames(path) if is6007(p) and len(p)>=32]
    t0=fr[0][0]; mac2sid={}
    for t,p in fr:
        s=sysid(p[24:30])
        if s: mac2sid.setdefault(bytes(p[6:12]),s)
    print(path.split('/')[-1])
    for t,p in fr:
        s=sysid(p[24:30]); d=mac2sid.get(bytes(p[0:6]))
        if p[30] in (0x4b,0x5b) and len(p)>=BODY+20 and p[BODY+8]==1 and p[BODY+9]==1:
            b=p[BODY:]
            print("  t=%8.3f %d->%s b12=%02x b18=%02x b22=%s b28=%s b36=%s b44=%s b56=%s b84=%s"%(t-t0,s,d,b[12],b[18],b[22:24].hex(),b[28:36].hex(),b[36:44].hex(),b[44:48].hex(),b[56:60].hex(),b[84:88].hex()))
