#!/usr/bin/env python3
# directed-HELLO decoder for ethertype 0x6007 (DEC SCA/LAVC).
# Pure pcap parse (no tshark). Extracts abs-30 per-frame word, abs-16 dest-logical,
# abs-24 src-logical, abs-92 incarnation, MACs. Clean-room: observation only.
import sys, struct

PFW = {0xa0:'a0(mcast)',0xb1:'b1(lastgasp)',0xb2:'b2-INIT',0xb3:'b3-REQ',0xb4:'b4-CONFIRM'}

def mac(b): return ':'.join('%02x'%x for x in b)

def read_pcap(path):
    with open(path,'rb') as f:
        gh=f.read(24)
        if len(gh)<24: return
        magic=struct.unpack('<I',gh[:4])[0]
        if magic in (0xa1b2c3d4,0xa1b23c4d): endi='<'
        elif magic in (0xd4c3b2a1,0x4d3cb2a1): endi='>'
        else: endi='<'
        nano = magic in (0xa1b23c4d,0x4d3cb2a1)
        while True:
            ph=f.read(16)
            if len(ph)<16: break
            ts_s,ts_f,incl,orig=struct.unpack(endi+'IIII',ph)
            data=f.read(incl)
            if len(data)<incl: break
            ts=ts_s + ts_f/(1e9 if nano else 1e6)
            yield ts,data

def decode(path, want_srcs=None, want_dsts=None, only_directed=True):
    rows=[]
    for ts,d in read_pcap(path):
        if len(d)<14: continue
        eth=struct.unpack('>H',d[12:14])[0]
        if eth!=0x6007: continue
        if len(d)<134: continue
        dst=d[0:6]; src=d[6:12]
        # message class byte abs-36
        mclass=d[36]
        pfw=d[30]; pfw_hi=d[31]
        dlog=d[16:22]; slog=d[24:30]
        inc=d[92] | (d[93]<<8)
        # HELLO message class = 0x05 at abs-36; only HELLOs carry the pfw channel-verify word + incarnation
        is_hello = (mclass==0x05)
        directed = is_hello and ((d[92]!=0 or d[93]!=0) or pfw in (0xb2,0xb3,0xb4,0xb1))
        if not is_hello: continue
        rows.append((ts,mac(src),mac(dst),pfw,pfw_hi,mac(dlog),mac(slog),inc,len(d),mclass,directed))
    return rows

def summarize(path, label):
    rows=decode(path)
    print("="*90)
    print(f"{label}\n  {path}")
    if not rows:
        print("  (no 0x6007 frames)"); return
    t0=rows[0][0]
    # per src->dst pfw histogram
    hist={}
    for r in rows:
        directed=r[10]
        key=(r[1],r[2])
        h=hist.setdefault(key,{'b2':0,'b3':0,'b4':0,'a0':0,'b1':0,'other':0,'inc':set(),'dlog':set(),'n':0})
        h['n']+=1
        p=r[3]
        if p==0xb2:h['b2']+=1
        elif p==0xb3:h['b3']+=1
        elif p==0xb4:h['b4']+=1
        elif p==0xa0:h['a0']+=1
        elif p==0xb1:h['b1']+=1
        else:h['other']+=1
        if directed:
            h['inc'].add(r[7]); h['dlog'].add(r[5])  # r[5]=abs16 dest-logical
    print(f"  frames={len(rows)} span={rows[-1][0]-t0:.1f}s  src->dst pfw histogram (b2/b3/b4 = channel-verify handshake):")
    for k,h in sorted(hist.items()):
        print(f"   {k[0]} -> {k[1]}  n={h['n']:4d}  b2={h['b2']:3d} b3={h['b3']:3d} b4={h['b4']:3d} a0={h['a0']:3d} b1={h['b1']:2d} other={h['other']:3d}  inc={sorted(h['inc'])} dlog={sorted(h['dlog'])}")

if __name__=='__main__':
    for p in sys.argv[1:]:
        summarize(p, p.split('/')[-1])
