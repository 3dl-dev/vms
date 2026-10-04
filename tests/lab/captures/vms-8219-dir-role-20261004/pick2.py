#!/usr/bin/env python3
"""pick2.py <L1.pcap> <L2.pcap> <out.pcap> -- the named op-0x04 removal of DLMTC (VAX3 -> directory VAX1, L1),
and the first op-0x0d registration VAX1 -> VAX2 in L2."""
import sys, struct
def recs(p):
    d=open(p,'rb').read(); off=24; hdr=d[:24]; out=[]
    while off+16<=len(d):
        h=d[off:off+16]; incl=struct.unpack('<I',h[8:12])[0]; out.append((h,d[off+16:off+16+incl])); off+=16+incl
    return hdr,out
hdr,r1=recs(sys.argv[1]); _,r2=recs(sys.argv[2]); keep=[]
for h,p in r1:
    if len(p)>=204 and p[12:14]==b'\x60\x07' and struct.unpack('<H',p[60:62])[0]==10:
        b=p[72:204]
        if b[8]==2 and b[9]==4 and b[48:48+b[47]]==b'DLMTC': keep.append(h+p); break
for h,p in r2:
    if len(p)>=204 and p[12:14]==b'\x60\x07' and struct.unpack('<H',p[60:62])[0]==10:
        b=p[72:204]
        if b[8]==2 and b[9]==0x0d and b[47]>0: keep.append(h+p); break
open(sys.argv[3],'wb').write(hdr+b''.join(keep)); print(len(keep))
