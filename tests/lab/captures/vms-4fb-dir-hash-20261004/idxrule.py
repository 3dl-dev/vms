#!/usr/bin/env python3
"""idxrule.py <pcap> <src-mac-suffix> <csv-ordered-vector-of-mac-suffixes,...>
For the FIRST op-01 request each name gets from <src>, which directory node did it go to, and does
dest == vector[f(hash) mod n] for f in candidate readings of the 32-bit body[128:132] value?"""
import sys, struct, collections
def frames(path):
    d=open(path,"rb").read(); off=24
    while off+16<=len(d):
        ts,tus,cl,_=struct.unpack("<IIII",d[off:off+16]); off+=16
        yield ts+tus/1e6, d[off:off+cl]; off+=cl
src=sys.argv[2]; vec=sys.argv[3].split(',')
seen=set(); rows=[]
for ts,f in frames(sys.argv[1]):
    if f[12:14]!=b"\x60\x07": continue
    c=f[14:]
    if len(c)<58+132 or struct.unpack("<H",c[46:48])[0]!=10: continue
    b=c[58:58+132]
    if b[8]!=0x02 or b[9]!=0x01 or b[46]!=0x03: continue
    if f[6:12].hex()[-4:]!=src: continue
    nl=b[47]; nm=b[48:48+nl]
    if nm in seen: continue
    seen.add(nm)
    rows.append((nm, struct.unpack('<I',b[128:132])[0], f[0:6].hex()[-4:]))
n=len(vec)
cands={'h32':lambda h:h,'lo16':lambda h:h&0xffff,'hi16':lambda h:h>>16,'b0':lambda h:h&0xff,'b3':lambda h:h>>24,'b1':lambda h:(h>>8)&0xff,'b2':lambda h:(h>>16)&0xff}
print("first-lookups:",len(rows), "dest histogram:", collections.Counter(r[2] for r in rows))
for k,fn in cands.items():
    ok=sum(1 for nm,h,d in rows if vec[fn(h)%n]==d)
    print("%-5s %d/%d"%(k,ok,len(rows)))
for r in rows[:15]: print("  %-24r %08x -> %s"%r)
print("hi16 mismatches:")
for nm,h,d in rows:
    if vec[(h>>16)%n]!=d: print("  %-30r %08x -> %s (hi16%%n=%d)"%(nm,h,d,(h>>16)%n))
