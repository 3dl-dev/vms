#!/usr/bin/env python3
"""rootchk.py <pcap>... -- op-01 requests: with body[36:44]==0 (no parent) is (mode,name)->body[128:132]
unique? and with body[36:44]!=0 how many (mode,name) carry >1 value?"""
import sys, struct, collections
def frames(path):
    d=open(path,"rb").read(); off=24
    while off+16<=len(d):
        ts,tus,cl,_=struct.unpack("<IIII",d[off:off+16]); off+=16
        yield ts+tus/1e6, d[off:off+cl]; off+=cl
grp={True:collections.defaultdict(set),False:collections.defaultdict(set)}; byname=collections.defaultdict(set)
for p in sys.argv[1:]:
  for ts,f in frames(p):
    if f[12:14]!=b"\x60\x07": continue
    c=f[14:]
    if len(c)<58+132 or struct.unpack("<H",c[46:48])[0]!=10: continue
    b=c[58:58+132]
    if b[8]!=2 or b[9]!=1 or not 0<b[47]<=31: continue
    root = b[36:44]==b'\0'*8
    nm=bytes(b[48:48+b[47]])
    grp[root][(b[46],nm)].add(b[128:132]); byname[nm].add((b[46],b[128:132]))
for r in (True,False):
    g=grp[r]; print("root" if r else "sub ", "names",len(g),"multi-valued",sum(1 for v in g.values() if len(v)>1))
modes=collections.Counter(k[0] for k in grp[True]); print("root modes",modes)
x=[nm for nm,v in byname.items() if len({m for m,h in v})>1]; print("names seen in >1 mode:",len(x), x[:5])
