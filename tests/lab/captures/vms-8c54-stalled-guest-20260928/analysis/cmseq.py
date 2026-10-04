#!/usr/bin/env python3
"""cmseq.py <pcap> <t0> <lo> <hi> -- the connection manager SYSAP body sequence
counters beside the 16-byte connect data, so the question "is conndata[12:14]
one of these counters?" is answered by reading, not by guessing.

CLEAN-ROOM: every offset is one already grounded in
docs/cluster-protocol-spec.md / vms_cluster_codec_cm.h --
  content[46:48]  SCA connection-control type (0 CONNECT_REQ, 2 ACCEPT_REQ,
                  10 application data)
  content[50:58]  the Con.ID pair
  content[62:78]  SYSAP name
  content[94:110] the 16-byte SCA connect data
  abs 72 / abs 74 the CM body send / ack message counters (LE u16)
"""
import sys, struct
def frames(path):
    d=open(path,"rb").read(); nano=d[:4] in (b"\x4d\x3c\xb2\xa1",b"\xa1\xb2\x3c\x4d"); off=24
    while off+16<=len(d):
        ts,tus,cl,_=struct.unpack("<IIII",d[off:off+16]); off+=16
        yield ts+tus/(1e9 if nano else 1e6), d[off:off+cl]; off+=cl
p,t0,lo,hi=sys.argv[1],float(sys.argv[2]),float(sys.argv[3]),float(sys.argv[4])
u16=lambda c,a: struct.unpack("<H", c[a-14:a-12])[0]
for ts,f in frames(p):
    t=ts-t0
    if not(lo<=t<=hi) or len(f)<14+62 or f[12:14]!=b"\x60\x07": continue
    c=f[14:]
    typ=u16(c,46+14) if False else struct.unpack("<H",c[46:48])[0]
    src=":".join("%02x"%b for b in f[9:12]); dst=":".join("%02x"%b for b in f[3:6])
    if typ in (0,2) and len(c)>=110:
        nm=bytes(c[62:78]).split(b"\x00")[0].decode("latin1").strip()
        if "VAXcluster" not in nm: continue
        print("t=%9.3f %s->%s %-11s loc=%s cd[12:14]=%02x %02x (=%5d)" % (
            t, src, dst, "CONNECT_REQ" if typ==0 else "ACCEPT_REQ",
            c[54:58][::-1].hex(), c[106], c[107],
            struct.unpack("<H", c[106:108])[0]))
    elif typ==10 and len(c)>=76:
        print("t=%9.3f %s->%s APPDATA     rem=%s loc=%s  CM send=%5d ack=%5d clen=%d" % (
            t, src, dst, c[50:54][::-1].hex(), c[54:58][::-1].hex(),
            struct.unpack("<H", c[72-14:74-14])[0],
            struct.unpack("<H", c[74-14:76-14])[0], len(c)))
