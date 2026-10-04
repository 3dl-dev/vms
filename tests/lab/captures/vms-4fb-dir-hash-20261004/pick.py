#!/usr/bin/env python3
"""pick.py <in.pcap> <out.pcap> -- keep: the op-01 requests + echoes for DLMTA (S1 vax2, S4 vax3) and the
first F11B$s sub-resource op-01 request + echo."""
import sys, struct
d=open(sys.argv[1],'rb').read(); off=24; out=[d[:24]]; reqs={}; want=[]; got_sub=False
recs=[]
while off+16<=len(d):
    h=d[off:off+16]; incl=struct.unpack('<I',h[8:12])[0]; p=d[off+16:off+16+incl]; off+=16+incl
    recs.append((h,p))
keep=set()
for i,(h,p) in enumerate(recs):
    if len(p)<204 or p[12:14]!=b'\x60\x07' or struct.unpack('<H',p[60:62])[0]!=10: continue
    b=p[72:204]
    if b[8]==2 and b[9]==1:
        nm=b[48:48+b[47]]
        sub = b[36:44]!=b'\0'*8
        if (nm==b'DLMTA' and not sub) or (sub and nm.startswith(b'F11B$s') and not got_sub):
            if sub: got_sub=True
            keep.add(i); reqs[(p[6:12],p[0:6],b[4:8])]=i
    elif b[8]==0x82 and b[9]==1:
        if (p[0:6],p[6:12],b[4:8]) in reqs: keep.add(i)
for i in sorted(keep): out.append(recs[i][0]+recs[i][1])
open(sys.argv[2],'wb').write(b''.join(out)); print(len(keep))
