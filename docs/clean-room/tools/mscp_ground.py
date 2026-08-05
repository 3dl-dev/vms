import sys
from pcap import frames, is6007
path='/home/baron/vax/cluster/captures/af2-firsttimer-established-20260728.pcap'
JOINER=bytes.fromhex('08002b7856b9'); VAX1=bytes.fromhex('aa0004000104')
def le16(b,o): return b[o]|(b[o+1]<<8)
def le32(b,o): return b[o]|(b[o+1]<<8)|(b[o+2]<<16)|(b[o+3]<<24)
def nm(p,o): return p[o:o+16].split(b'\x00')[0].decode('latin1','replace')
fr=[(t,p) for (t,p) in frames(path) if is6007(p)]
t0=fr[0][0]
# MSCP client conid pair: joiner 8fd20008 <-> VAX1 3554000a
CONS={0x8fd20008,0x3554000a}
print("=== ALL frames touching MSCP client conids 8fd20008/3554000a (t 143.8-146) ===")
for t,p in fr:
    dt=t-t0
    if dt<143.80 or dt>146.0: continue
    if len(p)<72 or p[30] not in (0x5b,0x4b): continue
    rc=le32(p,64); lc=le32(p,68)
    if not (rc in CONS or lc in CONS): continue
    d='J>V' if p[6:12]==JOINER else ('V>J' if p[6:12]==VAX1 else '?')
    op=le16(p,60)
    scalen=len(p)-14
    body=p[72:]
    print("%9.5f %s mt=%02x len=%3d op=%2d ss=%3d ra=%3d rc=%08x lc=%08x"%(
        dt,d,p[30],scalen,op,le16(p,34),le16(p,32),rc,lc))
    print("           body@72: %s"%(body.hex()))
