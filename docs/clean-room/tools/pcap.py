import struct, sys

def frames(path):
    with open(path,'rb') as f:
        data=f.read()
    magic=struct.unpack('<I',data[:4])[0]
    nano = (magic==0xa1b23c4d)
    off=24; out=[]
    while off+16<=len(data):
        ts_s,ts_u,incl,orig=struct.unpack('<IIII',data[off:off+16])
        off+=16
        pkt=data[off:off+incl]; off+=incl
        ts = ts_s + ts_u/(1e9 if nano else 1e6)
        out.append((ts,pkt))
    return out

def mac(b): return ':'.join('%02x'%x for x in b)

def hx(b): return b.hex()

def is6007(p): return len(p)>=14 and p[12:14]==b'\x60\x07'
