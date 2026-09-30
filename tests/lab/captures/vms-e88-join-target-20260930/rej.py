import sys, struct
sys.path.insert(0,'/root'); sys.path.insert(0,'docs/clean-room/tools')
from pcap import frames
def sysid(p): return p[28]|(p[29]<<8)
fr=[(t,p) for t,p in frames(sys.argv[1]) if len(p)>=76 and p[12:14]==b'\x60\x07']
t0=fr[0][0]; mac={}
for t,p in fr:
    if sysid(p): mac.setdefault(bytes(p[6:12]),sysid(p))
name={}
for t,p in fr:
    mt=struct.unpack('<H',p[60:62])[0]
    if mt in (0,2) and len(p)>=124:
        name[struct.unpack('<I',p[68:72])[0]]=p[76:92].decode('ascii','replace').strip()
for t,p in fr:
    mt=struct.unpack('<H',p[60:62])[0]
    if mt==4:
        rem,loc=struct.unpack('<II',p[64:72])
        print("t=%.3f %s->%s REJECT_REQ rem=%08x (%s) [58:62]=%s"%(t-t0,sysid(p),mac.get(bytes(p[0:6])),rem,name.get(rem,'?'),p[72:76].hex()))
