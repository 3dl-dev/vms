import sys,struct
# reord.py rx-pcap me : find sequenced frames TO `me` that arrive out of order; print context incl. me's acks
d=open(sys.argv[1],"rb").read(); off=24; me=sys.argv[2]; fr=[]; base=None
while off+16<=len(d):
    ts,tus,cl,_=struct.unpack("<IIII",d[off:off+16]); off+=16; f=d[off:off+cl]; off+=cl
    if f[12:14]!=b"\x60\x07" or len(f)<36: continue
    t=ts+tus/1e6
    if base is None: base=t
    s=f[6:12].hex()[-4:]; dd=f[0:6].hex()[-4:]
    fr.append((t-base,s,dd,f[30],struct.unpack("<H",f[32:34])[0],struct.unpack("<H",f[34:36])[0]))
last={}; hits=[]
for i,(t,s,dd,mt,ack,seq) in enumerate(fr):
    if dd!=me or mt not in (0x4b,0x5b): continue
    p=last.get(s)
    if p is not None and seq<p and (p-seq)<50: hits.append(i)
    last[s]=max(seq,p or 0)
print("out-of-order arrivals:",len(hits))
for i in hits[:6]:
    print("----")
    for (t,s,dd,mt,ack,seq) in fr[max(0,i-4):i+8]:
        if mt in (0xb3,0xb4): continue
        print("%9.3f %s>%s mt=%02x seq=%d ack=%d"%(t,s,dd,mt,seq,ack))
