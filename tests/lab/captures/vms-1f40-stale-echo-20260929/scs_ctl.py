#!/usr/bin/env python3
"""scs_ctl.py <pcap> <t0> <lo> <hi> [mac...] -- every SCA connection-control frame (all but application data) in a window, with sequence numbers, Con.IDs and SYSAP name.

CLEAN-ROOM: offsets only from docs/cluster-protocol-spec.md -- abs 30 message
type (0x4b/0x5b sequenced), content[46:48] SCA connection-control type,
content[50:54]/[54:58] remote/local Con.ID, content[62:78] SYSAP name (spec
4(h)(1a)/(2), 4(g)). Rig MACs: 52:54:00:00:df:0a OVMXA, :0b OVMXB,
08:00:2b:fb:91:86 VAXC. Inputs: <run-dir>/fault.out + s8.pcap.gz (never
committed).
"""
import sys,struct
NAMES={0:"CONNECT_REQ",1:"CONNECT_RSP",2:"ACCEPT_REQ",3:"ACCEPT_RSP",4:"REJECT_REQ",5:"REJECT_RSP",6:"DISCONNECT_REQ",7:"DISCONNECT_RSP",10:"APPDATA",8:"CREDIT_REQ",9:"CREDIT_RSP"}
def frames(path):
    d=open(path,"rb").read(); nano=d[:4]==b"\x4d\x3c\xb2\xa1"; off=24
    while off+16<=len(d):
        ts,tus,cl,_=struct.unpack("<IIII",d[off:off+16]); off+=16
        yield ts+tus/(1e9 if nano else 1e6), d[off:off+cl]; off+=cl
p,t0,lo,hi=sys.argv[1],float(sys.argv[2]),float(sys.argv[3]),float(sys.argv[4])
macs=set(sys.argv[5:])
for ts,f in frames(p):
    t=ts-t0
    if not(lo<=t<=hi) or f[12:14]!=b"\x60\x07" or len(f)<14+62: continue
    s=f[6:12].hex(); d=f[0:6].hex()
    if macs and not (s in macs and d in macs): continue
    if f[30] not in (0x4b,0x5b,0x7b): continue
    c=f[14:]; ty=struct.unpack("<H",c[46:48])[0]
    if ty in (10,): continue
    nm=bytes(c[62:78]).split(b"\x00")[0].decode("latin1").strip() if ty in (0,2) else ""
    print("%8.3f %s->%s mt=%02x ack=%d seq=%d %-14s rem=%s loc=%s %s"%(t,s[-4:],d[-4:],f[30],struct.unpack("<H",f[32:34])[0],struct.unpack("<H",f[34:36])[0],NAMES.get(ty,str(ty)),c[50:54][::-1].hex(),c[54:58][::-1].hex(),nm))
