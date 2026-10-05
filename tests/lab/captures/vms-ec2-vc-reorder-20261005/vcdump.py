#!/usr/bin/env python3
"""vcdump.py <pcap> <macA4hex> <macB4hex> [t0 t1] -- per-frame VC/SCS view of the A<->B pair"""
import sys, struct
def frames(path):
    d=open(path,"rb").read(); off=24
    while off+16<=len(d):
        ts,tus,cl,_=struct.unpack("<IIII",d[off:off+16]); off+=16
        yield ts+tus/1e6, d[off:off+cl]; off+=cl
a,b=sys.argv[2],sys.argv[3]
t0=float(sys.argv[4]) if len(sys.argv)>4 else 0; t1=float(sys.argv[5]) if len(sys.argv)>5 else 1e18
base=None
for ts,f in frames(sys.argv[1]):
    if f[12:14]!=b"\x60\x07": continue
    if base is None: base=ts
    t=ts-base
    if t<t0 or t>t1: continue
    s=f[6:12].hex()[-4:]; d=f[0:6].hex()[-4:]
    if {s,d}!={a,b}: continue
    mt=f[30] if len(f)>30 else -1
    ack=struct.unpack("<H",f[32:34])[0] if len(f)>=36 else -1
    seq=struct.unpack("<H",f[34:36])[0] if len(f)>=36 else -1
    x=""
    if len(f)>=72 and mt in (0x4b,0x5b,0x7b):
        smt,cr=struct.unpack("<HH",f[60:64])
        x="scsmt=%d cr=%d"%(smt,cr)
        if smt==10 and len(f)>=90:
            body=f[72:]
            x+=" cat=%02x op=%02x sendno=%d"%(body[8],body[9],struct.unpack("<H",body[0:2])[0])
    print("%8.3f %s>%s len=%d mt=%02x seq=%d ack=%d %s"%(t,s,d,len(f),mt,seq,ack,x))
