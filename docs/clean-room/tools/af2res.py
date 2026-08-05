import sys
from pcap import frames, is6007
path=sys.argv[1]
JOINER=bytes.fromhex('08002b7856b9'); VAX1=bytes.fromhex('aa0004000104')
def le16(b,o): return b[o]|(b[o+1]<<8)
def le32(b,o): return b[o]|(b[o+1]<<8)|(b[o+2]<<16)|(b[o+3]<<24)
fr=[(t,p) for (t,p) in frames(path) if is6007(p)]
t0=fr[0][0]
# The JOINER's HIT responses (J>M, op=10, marker@72==1) for MSCP$DISK and VMS$VAXcluster, first occurrence
want={b'MSCP$DISK':0,b'VMS$VAXcluster':0}
for t,p in fr:
    dt=t-t0
    if dt<143.0 or dt>144.0: continue
    if len(p)<108 or p[30] not in (0x5b,0x4b): continue
    if le16(p,60)!=10: continue
    if p[6:12]!=JOINER: continue     # joiner is the SERVER answering
    if le32(p,72)!=1: continue        # marker=1 => response
    nm=p[76:92].split(b'\x00')[0]
    for k in want:
        if nm.startswith(k) and want[k]<1:
            want[k]=1
            print("JOINER HIT resp for %s @%.4f:"%(k.decode(),dt))
            print("  name@76 :", p[76:92].hex())
            print("  result@92:", p[92:108].hex(), repr(p[92:108]))
