#!/usr/bin/env python3
"""mt_census.py <pcap> <t0> <lo> <hi> [srcfilter] -- every 0x6007 frame in a
window by abs-30 SCA/port function word (docs/cluster-protocol-spec.md 4(b)/
4(g)/4(O.30)). Nothing is interpreted beyond the one grounded offset."""
import sys, struct
def frames(path):
    d=open(path,"rb").read(); nano=d[:4] in (b"\x4d\x3c\xb2\xa1",b"\xa1\xb2\x3c\x4d"); off=24
    while off+16<=len(d):
        ts,tus,cl,_=struct.unpack("<IIII",d[off:off+16]); off+=16
        yield ts+tus/(1e9 if nano else 1e6), d[off:off+cl]; off+=cl
p,t0,lo,hi=sys.argv[1],float(sys.argv[2]),float(sys.argv[3]),float(sys.argv[4])
pat=sys.argv[5] if len(sys.argv)>5 else ""
for ts,f in frames(p):
    t=ts-t0
    if not(lo<=t<=hi) or len(f)<46 or f[12:14]!=b"\x60\x07": continue
    src=":".join("%02x"%b for b in f[6:12]); dst=":".join("%02x"%b for b in f[0:6])
    if pat and pat not in src and pat not in dst: continue
    print("t=%9.3f %s -> %s  abs30=0x%02x len=%d"%(t,src[9:],dst[9:],f[14+16],len(f)))
