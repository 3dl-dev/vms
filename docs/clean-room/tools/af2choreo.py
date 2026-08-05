import struct, sys
from pcap import frames, is6007
path=sys.argv[1]
JOINER=bytes.fromhex('08002b7856b9'); VAX1=bytes.fromhex('aa0004000104')
def le16(b,o): return b[o]|(b[o+1]<<8)
def le32(b,o): return b[o]|(b[o+1]<<8)|(b[o+2]<<16)|(b[o+3]<<24)
SYSAPS=[b'SCS$DIRECTORY',b'MSCP$DISK',b'MSCP$TAPE',b'VMS$VAXcluster',b'SCS$DIR_LOOKUP',b'VMS$DISK_CL_DRVR',b'NOT PRESENT']
def name_at(p,o):
    s=p[o:o+16]
    return s.split(b'\x00')[0].decode('latin1','replace')
fr=[(t,p) for (t,p) in frames(path) if is6007(p)]
t0=fr[0][0]
rows=[]
for i,(t,p) in enumerate(fr):
    if len(p)<94: continue
    mt=p[30]
    if mt not in (0x5b,0x4b): continue
    # does it carry a sysap name?
    if not any(s in p for s in SYSAPS): continue
    d='J>M' if p[6:12]==JOINER else ('M>J' if p[6:12]==VAX1 else '?>?')
    op=le16(p,60); ra=le16(p,32); ss=le16(p,34)
    rc=le32(p,64); lc=le32(p,68); mk=le32(p,72)
    nm=name_at(p,76); res=name_at(p,92)
    scalen=len(p)-14
    rows.append((t-t0,d,'%02x'%mt,scalen,op,ss,ra,'%08x'%rc,'%08x'%lc,'%08x'%mk,nm,res))
print("named-SYSAP frames:", len(rows))
print("  t     dir  mt len op  ss  ra  rconid   lconid   marker   name / result")
for r in rows:
    print("%8.3f %s %s %3d %2d %3d %3d %s %s %s  %-14s | %s"%r)
