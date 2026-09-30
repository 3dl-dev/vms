#!/usr/bin/env python3
import sys, struct
def frames(path):
    d=open(path,"rb").read(); nano=d[:4] in (b"\x4d\x3c\xb2\xa1",b"\xa1\xb2\x3c\x4d"); off=24
    while off+16<=len(d):
        ts,tus,cl,_=struct.unpack("<IIII",d[off:off+16]); off+=16
        yield ts+tus/(1e9 if nano else 1e6), d[off:off+cl]; off+=cl
u16=lambda c,a: struct.unpack("<H", c[a-14:a-12])[0]
from collections import Counter
cnt=Counter()
for ts,f in frames(sys.argv[1]):
    if len(f)<14+120 or f[12:14]!=b"\x60\x07": continue
    c=f[14:]
    if struct.unpack("<H",c[46:48])[0]!=10 or len(c)<120: continue
    if c[81-14]!=0x0d: continue
    cnt[(("%02x"%c[80-14]), ":".join("%02x"%b for b in f[9:12]), u16(c,84), u16(c,86))]+=1
for k,v in sorted(cnt.items(), key=lambda x:-x[1]):
    print("cat=%s src=%s L1tag=%04x tag2=%04x  n=%d"%(k[0],k[1],k[2],k[3],v))
