#!/usr/bin/env python3
"""conndata.py <pcap> <sysap> -- the 16-byte SCA connect data (content[94:110],
docs/cluster-protocol-spec.md 4(h)(2)) of every CONNECT_REQ/ACCEPT_REQ for one
SYSAP, with the connection-control type at content[46:48] and the Con.ID pair
at content[50:58]. Offsets already grounded; nothing else is interpreted."""
import sys, struct
def frames(path):
    d=open(path,"rb").read(); nano=d[:4] in (b"\x4d\x3c\xb2\xa1",b"\xa1\xb2\x3c\x4d"); off=24
    while off+16<=len(d):
        ts,tus,cl,_=struct.unpack("<IIII",d[off:off+16]); off+=16
        yield ts+tus/(1e9 if nano else 1e6), d[off:off+cl]; off+=cl
NAMES={0:"CONNECT_REQ",1:"CONNECT_RSP",2:"ACCEPT_REQ",3:"ACCEPT_RSP",4:"REJECT_REQ",
       5:"REJECT_RSP",6:"DISCONNECT_REQ",7:"DISCONNECT_RSP",10:"APPDATA"}
p=sys.argv[1]; want=sys.argv[2] if len(sys.argv)>2 else "VMS$VAXcluster"
for ts,f in frames(p):
    if len(f)<14+110 or f[12:14]!=b"\x60\x07": continue
    c=f[14:]
    t=struct.unpack("<H",c[46:48])[0]
    if t not in (0,2): continue
    n1=bytes(c[62:78]).split(b"\x00")[0].decode("latin1").strip()
    if want not in n1: continue
    print("%.6f %s -> %s %-11s rem=%s loc=%s conndata=%s" % (
        ts, ":".join("%02x"%b for b in f[9:12]), ":".join("%02x"%b for b in f[3:6]),
        NAMES.get(t,str(t)),
        c[50:54][::-1].hex(), c[54:58][::-1].hex(),
        " ".join("%02x"%b for b in c[94:110])))
