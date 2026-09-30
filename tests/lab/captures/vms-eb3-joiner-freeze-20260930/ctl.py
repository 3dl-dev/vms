import sys, struct
sys.path.insert(0,'/root')
from pcap import frames
fr=list(frames(sys.argv[1])); t0=fr[0][0]; lo,hi=float(sys.argv[2]),float(sys.argv[3])
for i,(t,p) in enumerate(fr):
    if not (lo<=t-t0<=hi) or len(p)<62 or p[12:14]!=b'\x60\x07': continue
    if p[30] not in (0x4b,0x5b): continue
    mt=struct.unpack('<H',p[60:62])[0]
    if mt==10: continue
    s=p[28]|(p[29]<<8)
    print(i,'t=%.3f'%(t-t0),'src',s,'mt',mt,'len',len(p),p[64:].hex())
