import sys
from pcap import frames, is6007
path=sys.argv[1]
JOINER=bytes.fromhex('08002b7856b9'); VAX1=bytes.fromhex('aa0004000104')
def le16(b,o): return b[o]|(b[o+1]<<8)
def le32(b,o): return b[o]|(b[o+1]<<8)|(b[o+2]<<16)|(b[o+3]<<24)
def nm(p,o): return p[o:o+16].split(b'\x00')[0].decode('latin1','replace')
fr=[(t,p) for (t,p) in frames(path) if is6007(p)]
t0=fr[0][0]
# The member's MSCP$DISK connect (M>J op=0 name=MSCP$DISK ss=7) and the following
# joiner accept frames (J>M op=1 echo, op=2 response) on that Con.ID pair.
# member local conid was 3553000a (from earlier). Track by conid pair.
print("=== member MSCP$DISK connect + joiner accept (t 143.75-143.90) ===")
for t,p in fr:
    dt=t-t0
    if dt<143.75 or dt>143.90: continue
    if len(p)<72 or p[30] not in (0x5b,0x4b): continue
    op=le16(p,60)
    rc=le32(p,64); lc=le32(p,68)
    # frames on the MSCP conid pair (member 3553000a) or its accept
    involved = (0x3553000a in (rc,lc))
    nmv=nm(p,76) if len(p)>=92 else ''
    if not (involved or (op in (0,1,2) and nmv.startswith('MSCP$DISK'))): continue
    d='M>J' if p[6:12]==VAX1 else 'J>M'
    print("%8.4f %s mt=%02x len=%3d op=%d ss=%3d ra=%3d rc=%08x lc=%08x name=%-14s res=%s"%(
        dt,d,p[30],len(p)-14,op,le16(p,34),le16(p,32),rc,lc,nmv,nm(p,92) if len(p)>=108 else ''))
