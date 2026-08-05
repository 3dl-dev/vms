import sys
from pcap import frames, is6007
path=sys.argv[1]
JOINER=bytes.fromhex('08002b7856b9'); VAX1=bytes.fromhex('aa0004000104')
def le16(b,o): return b[o]|(b[o+1]<<8)
def le32(b,o): return b[o]|(b[o+1]<<8)|(b[o+2]<<16)|(b[o+3]<<24)
def nm(p,o): return p[o:o+16].split(b'\x00')[0].decode('latin1','replace')
fr=[(t,p) for (t,p) in frames(path) if is6007(p)]
t0=fr[0][0]
# All op=0 CONNECT-REQUEST frames (msgtype 5b/4b, op@60==0) in the first join window (143.0-144.0)
print("=== ALL op=0 connect-requests, t in [143.0,144.5] ===")
for t,p in fr:
    dt=t-t0
    if dt<143.0 or dt>144.5: continue
    if len(p)<80: continue
    if p[30] not in (0x5b,0x4b): continue
    if le16(p,60)!=0: continue
    d='VAX1>JOIN' if p[6:12]==VAX1 else ('JOIN>VAX1' if p[6:12]==JOINER else '?')
    print("%8.4f %s mt=%02x len=%d ss=%d ra=%d lconid=%08x  name=%-14s res=%s"%(
        dt,d,p[30],len(p)-14,le16(p,34),le16(p,32),le32(p,68),nm(p,76),nm(p,92)))
# who sent the FIRST 5b/4b op-bearing frame after START(143.106)?
print("=== first 8 connect-class (5b or 4b w/ op in 0,1,2,3,10) frames after 143.11 ===")
c=0
for t,p in fr:
    dt=t-t0
    if dt<143.11: continue
    if len(p)<62 or p[30] not in (0x5b,0x4b): continue
    op=le16(p,60)
    if op not in (0,1,2,3,10): continue
    d='VAX1>JOIN' if p[6:12]==VAX1 else 'JOIN>VAX1'
    print("%8.4f %s mt=%02x op=%d len=%d ss=%d name=%s"%(dt,d,p[30],op,len(p)-14,le16(p,34),nm(p,76)))
    c+=1
    if c>=8: break
