import struct, sys
from pcap import frames, is6007
path=sys.argv[1]
JOINER=bytes.fromhex('08002b7856b9'); VAX1=bytes.fromhex('aa0004000104'); MC=bytes.fromhex('ab0004010101')
def le16(b,o): return b[o]|(b[o+1]<<8)
def le32(b,o): return b[o]|(b[o+1]<<8)|(b[o+2]<<16)|(b[o+3]<<24)
fr=[(t,p) for (t,p) in frames(path) if is6007(p)]
t0=fr[0][0]
def who(p):
    s=p[6:12]
    return 'JOIN' if s==JOINER else ('VAX1' if s==VAX1 else 'MC?')
def dst(p):
    d=p[0:6]
    return 'JOIN' if d==JOINER else ('VAX1' if d==VAX1 else ('MCAST' if d==MC else '?'))
# First: show all 0x41 START frames with timing
print("=== 0x41 START frames ===")
for t,p in fr:
    if len(p)>=31 and p[30]==0x41:
        print("%8.3f %s>%s  ss=%d ra=%d  abs22=%d abs36hex=%s"%(t-t0,who(p),dst(p),le16(p,34),le16(p,32),le16(p,22),p[36:40].hex()))
# Timeline of first 2.2s: every non-0x4b-keepalive frame (i.e. connect/dir/start/hello classes)
print("=== first 2.2s: msgtype events (excluding pure 190-byte keepalive noise) ===")
n=0
for t,p in fr:
    dt=t-t0
    if dt>2.2: break
    mt=p[30] if len(p)>=31 else 0
    scalen=len(p)-14
    # show START(41), directory/connect(5b/4b w/ op), HELLO(a0/... 134-byte)
    if mt in (0x41,0x5b) or (mt==0x4b and scalen in (94,110,66,62)) or scalen==134:
        op=le16(p,60) if len(p)>=62 else -1
        tag='HELLO' if scalen==134 else ('mt%02x'%mt)
        print("%8.4f %s>%s %s len=%d op=%d ss=%d ra=%d"%(dt,who(p),dst(p),tag,scalen,op,le16(p,34),le16(p,32)))
        n+=1
    if n>60: break
