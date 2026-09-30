import sys, struct
sys.path.insert(0,'/root')
from pcap import frames
fr=list(frames(sys.argv[1])); t0=fr[0][0]
mac={}
for i,(t,p) in enumerate(fr):
    if len(p)<82 or p[12:14]!=b'\x60\x07': continue
    s=p[28]|(p[29]<<8)
    if s: mac.setdefault(bytes(p[6:12]),s)
    if p[30] not in (0x4b,0x5b) or struct.unpack('<H',p[60:62])[0]!=10: continue
    d=mac.get(bytes(p[0:6])); cat,op=p[80],p[81]
    if {s,d}!={1026,1027}: continue
    if (cat,op) in ((1,9),(0x81,9),(1,0xa),(1,0xc),(1,0xb),(0x81,0xb),(6,0),(0x86,0)) :
        r,l=struct.unpack('<II',p[64:72])
        print('%5d t=%.3f %d->%d loc=%08x cat=%02x op=%02x smsg=%d amsg=%d len=%d'%(i,t-t0,s,d,l,cat,op,p[72]|p[73]<<8,p[74]|p[75]<<8,len(p)))
