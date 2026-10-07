#!/usr/bin/env python3
"""hashchk2.py <pcap>... -- per (cat,op): for frames with a name (len 1..31), is body[128:132] consistent
with the op-01-REQUEST-learned value for that name? (op-01 requests define the reference)"""
import sys, struct, collections
def frames(path):
    d=open(path,"rb").read(); off=24
    while off+16<=len(d):
        ts,tus,cl,_=struct.unpack("<IIII",d[off:off+16]); off+=16
        yield ts+tus/1e6, d[off:off+cl]; off+=cl
ref={}; allf=[]
for p in sys.argv[1:]:
  for ts,f in frames(p):
    if f[12:14]!=b"\x60\x07": continue
    c=f[14:]
    if len(c)<58+132 or struct.unpack("<H",c[46:48])[0]!=10: continue
    b=c[58:58+132]
    if (b[8]&0x7f)!=2: continue
    nl=b[47]
    if not 0<nl<=31: continue
    nm=(b[46],bytes(b[48:48+nl])); h=b[128:132].hex()
    allf.append((b[8],b[9],nm,h))
    if b[8]==2 and b[9]==1:
        ref.setdefault(nm,set()).add(h)
print("op-01 request names:",len(ref)," with >1 value:",sum(1 for v in ref.values() if len(v)>1))
st=collections.defaultdict(lambda:[0,0,0])
for cat,op,nm,h in allf:
    k=(cat,op); 
    if nm not in ref: st[k][2]+=1
    elif h in ref[nm]: st[k][0]+=1
    else: st[k][1]+=1
for k in sorted(st): print("cat %02x op %02x  agree %6d  DISAGREE %6d  name-not-seen-in-op01 %6d"%(k[0],k[1],*st[k]))
for nm,v in ref.items():
    if len(v)>1: print("  multi:",nm,sorted(v))
