#!/usr/bin/env python3
"""crossing.py <run-dir> -- the VMS$VAXcluster connection-control dialogue between OVMXB and the VAX after the wake, per Con.ID: shows which of two crossing connections the real VAX disconnected.

CLEAN-ROOM: offsets only from docs/cluster-protocol-spec.md -- abs 30 message
type (0x4b/0x5b sequenced), content[46:48] SCA connection-control type,
content[50:54]/[54:58] remote/local Con.ID, content[62:78] SYSAP name (spec
4(h)(1a)/(2), 4(g)). Rig MACs: 52:54:00:00:df:0a OVMXA, :0b OVMXB,
08:00:2b:fb:91:86 VAXC. Inputs: <run-dir>/fault.out + s8.pcap.gz (never
committed).
"""
import sys,struct,gzip,re,datetime
M={'52540000df0b':'B','52540000df0a':'A','08002bfb9186':'VAX'}
NAMES={0:"CONNECT_REQ",2:"ACCEPT_REQ",3:"ACCEPT_RSP",6:"DISCONNECT_REQ"}
d=sys.argv[1]
fo=open(d+'/fault.out').read().replace('\n',' ')
m=re.search(r"STALL off .*? at (\d\d:\d\d:[\d.]+) after",fo)
raw=gzip.open(d+'/s8.pcap.gz').read();nano=raw[:4]==b"\x4d\x3c\xb2\xa1";off=24;fr=[]
while off+16<=len(raw):
    ts,tu,cl,_=struct.unpack("<IIII",raw[off:off+16]);off+=16;fr.append((ts+tu/(1e9 if nano else 1e6),raw[off:off+cl]));off+=cl
day=datetime.datetime.fromtimestamp(fr[0][0],datetime.timezone.utc).strftime('%Y-%m-%d')
wake=datetime.datetime.strptime(day+' '+m.group(1),'%Y-%m-%d %H:%M:%S.%f').replace(tzinfo=datetime.timezone.utc).timestamp()
seen=set()
cm=set()
for ts,f in fr:
    if ts<wake or ts>wake+30 or f[12:14]!=b"\x60\x07" or len(f)<14+60 or f[30] not in (0x4b,0x5b): continue
    c=f[14:];ty=struct.unpack("<H",c[46:48])[0]
    if ty not in NAMES: continue
    s,dd=M.get(f[6:12].hex(),'?'),M.get(f[0:6].hex(),'?')
    if set((s,dd))!={'B','VAX'}: continue
    rem,loc=c[50:54][::-1].hex(),c[54:58][::-1].hex()
    nm=bytes(c[62:78]).split(b"\x00")[0].decode('latin1').strip() if ty in (0,2) else ''
    if ty==0 and 'VAXcluster' in nm: cm.add(loc)
    if ty==2 and 'VAXcluster' in nm: cm.add(rem); cm.add(loc)
    if ty in (3,6) and not (rem in cm or loc in cm): continue
    if ty in (0,2) and 'VAXcluster' not in nm: continue
    key=(s,ty,rem,loc)
    if key in seen: continue
    seen.add(key); print("  %+.2f %s>%s %-14s rem=%s loc=%s"%(ts-wake,s,dd,NAMES[ty],rem,loc))
