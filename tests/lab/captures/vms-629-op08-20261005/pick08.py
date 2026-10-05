#!/usr/bin/env python3
"""pick08.py <in.pcap> <out.pcap> -- keep the cat-0x02 op-0x08 SYS$SYS_ID request(s) and the
cat-0x82 answer(s) to them (same SYS$SYS_ID name), verbatim records."""
import sys, struct
d=open(sys.argv[1],'rb').read(); hdr=d[:24]; off=24; keep=[]
while off+16<=len(d):
    h=d[off:off+16]; incl=struct.unpack('<I',h[8:12])[0]; p=d[off+16:off+16+incl]; off+=16+incl
    if len(p)>=204 and p[12:14]==b'\x60\x07' and p[30] in (0x4b,0x5b,0x7b) and struct.unpack('<H',p[60:62])[0]==10:
        b=p[72:204]
        if b'SYS$SYS_ID' in b[48:64] and ((b[8],b[9])==(2,8) or (b[8],b[9])==(0x82,1)): keep.append(h+p)
open(sys.argv[2],'wb').write(hdr+b''.join(keep)); print(len(keep))
