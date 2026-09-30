#!/usr/bin/env python3
"""who_dials.py <run-dir> -- after the stall wakes, who dialled the VMS$VAXcluster connection, and what answered it (first 10 distinct connection-control frames involving OVMXB).

CLEAN-ROOM: offsets only from docs/cluster-protocol-spec.md -- abs 30 message
type (0x4b/0x5b sequenced), content[46:48] SCA connection-control type,
content[50:54]/[54:58] remote/local Con.ID, content[62:78] SYSAP name (spec
4(h)(1a)/(2), 4(g)). Rig MACs: 52:54:00:00:df:0a OVMXA, :0b OVMXB,
08:00:2b:fb:91:86 VAXC. Inputs: <run-dir>/fault.out + s8.pcap.gz (never
committed).
"""
import sys,struct,gzip,re,datetime
NAMES={0:"CONNECT_REQ",2:"ACCEPT_REQ",6:"DISCONNECT_REQ",4:"REJECT_REQ"}
M={'52540000df0b':'B','52540000df0a':'A','08002bfb9186':'VAX'}
d=sys.argv[1]
fo=open(d+'/fault.out').read().replace('\n',' ')
m=re.search(r"STALL off .*? at (\d\d:\d\d:[\d.]+) after",fo)
raw=gzip.open(d+'/s8.pcap.gz').read();nano=raw[:4]==b"\x4d\x3c\xb2\xa1";off=24;fr=[]
while off+16<=len(raw):
    ts,tu,cl,_=struct.unpack("<IIII",raw[off:off+16]);off+=16;fr.append((ts+tu/(1e9 if nano else 1e6),raw[off:off+cl]));off+=cl
day=datetime.datetime.fromtimestamp(fr[0][0],datetime.timezone.utc).strftime('%Y-%m-%d')
wake=datetime.datetime.strptime(day+' '+m.group(1),'%Y-%m-%d %H:%M:%S.%f').replace(tzinfo=datetime.timezone.utc).timestamp()
seen=set();out=[]
for ts,f in fr:
    if ts<wake or f[12:14]!=b"\x60\x07" or len(f)<14+78 or f[30] not in (0x4b,0x5b): continue
    c=f[14:];ty=struct.unpack("<H",c[46:48])[0]
    if ty not in NAMES: continue
    s,dd=M.get(f[6:12].hex(),'?'),M.get(f[0:6].hex(),'?')
    if 'B' not in (s,dd): continue
    nm=bytes(c[62:78]).split(b"\x00")[0].decode('latin1').strip() if ty in (0,2) else ''
    if ty in (0,2) and 'VAXcluster' not in nm: continue
    key=(s,dd,ty,c[50:58].hex())
    if key in seen: continue
    seen.add(key); out.append("%+.2f %s>%s %s"%(ts-wake,s,dd,NAMES[ty]))
print(d.split('/')[-1], " | ".join(out[:10]))
