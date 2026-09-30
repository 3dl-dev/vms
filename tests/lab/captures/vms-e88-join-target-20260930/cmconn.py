import sys, struct
sys.path.insert(0,'/root'); sys.path.insert(0,'docs/clean-room/tools')
from pcap import frames, is6007
BODY=72
def sysid(l): return l[4] | (l[5] << 8)
path=sys.argv[1]; a=int(sys.argv[2]); b=int(sys.argv[3])
fr=[(t,p) for (t,p) in frames(path) if is6007(p) and len(p)>=32]
t0=fr[0][0]; mac2sid={}
for t,p in fr:
    s=sysid(p[24:30])
    if s: mac2sid.setdefault(bytes(p[6:12]),s)
for t,p in fr:
    s=sysid(p[24:30]); d=mac2sid.get(bytes(p[0:6]))
    if {s,d}!={a,b}: continue
    if p[30] not in (0x4b,0x5b) or len(p)<BODY+20: continue
    mt=struct.unpack('<H',p[60:62])[0]
    if mt==10:
        r,l=struct.unpack('<II',p[64:72])
        print("t=%8.3f %d->%d rem=%08x loc=%08x cat=%02x op=%02x smsg=%d amsg=%d"%(t-t0,s,d,r,l,p[BODY+8],p[BODY+9],p[BODY]|p[BODY+1]<<8,p[BODY+2]|p[BODY+3]<<8))
    else:
        r,l=struct.unpack('<II',p[64:72])
        print("t=%8.3f %d->%d CTL mt=%d rem=%08x loc=%08x"%(t-t0,s,d,mt,r,l))
