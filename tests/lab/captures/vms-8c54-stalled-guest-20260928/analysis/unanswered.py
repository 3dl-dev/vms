#!/usr/bin/env python3
"""unanswered.py <pcap> -- which cat=02 op=0d requests never got a cat=82 op=0d
answer, matched on the (txn, token) pair the responder must echo.

Offsets are the already-grounded CM envelope (abs 72/74/76/78 + category at 80,
opcode at 81, vms_cluster_codec_cm.h). Nothing is interpreted beyond them.
"""
import sys, struct
def frames(path):
    d=open(path,"rb").read(); nano=d[:4] in (b"\x4d\x3c\xb2\xa1",b"\xa1\xb2\x3c\x4d"); off=24
    while off+16<=len(d):
        ts,tus,cl,_=struct.unpack("<IIII",d[off:off+16]); off+=16
        yield ts+tus/(1e9 if nano else 1e6), d[off:off+cl]; off+=cl
u16=lambda c,a: struct.unpack("<H", c[a-14:a-12])[0]
reqs=[]; answered=set()
for ts,f in frames(sys.argv[1]):
    if len(f)<14+82 or f[12:14]!=b"\x60\x07": continue
    c=f[14:]
    if struct.unpack("<H",c[46:48])[0]!=10 or len(c)<82: continue
    cat,op=c[80-14],c[81-14]
    if op!=0x0d: continue
    key=(u16(c,76),u16(c,78))
    if cat==0x02: reqs.append((ts,key,u16(c,72),":".join("%02x"%b for b in f[9:12])))
    elif cat==0x82: answered.add(key)
miss=[r for r in reqs if r[1] not in answered]
print("requests=%d answered-keys=%d UNANSWERED=%d"%(len(reqs),len(answered),len(miss)))
for ts,key,send,src in miss[:20]:
    print("  epoch=%.3f src=%s txn=%d tok=%d send=%d"%(ts,src,key[0],key[1],send))
