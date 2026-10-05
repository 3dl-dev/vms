import sys,struct
# pairre.py pcap src dst : for back-to-back sequenced frames src->dst (<5ms apart, consecutive seq),
# was the SECOND later retransmitted (0x7b/0x6b same seq)?
d=open(sys.argv[1],"rb").read(); off=24; a,b=sys.argv[2],sys.argv[3]; fr=[]
while off+16<=len(d):
    ts,tus,cl,_=struct.unpack("<IIII",d[off:off+16]); off+=16; f=d[off:off+cl]; off+=cl
    if f[12:14]!=b"\x60\x07" or len(f)<36: continue
    if f[6:12].hex()[-4:]!=a or f[0:6].hex()[-4:]!=b: continue
    if f[30] not in (0x4b,0x5b,0x7b,0x6b): continue
    fr.append((ts+tus/1e6, struct.unpack("<H",f[34:36])[0], f[30]))
pairs=0; re2=0
for i in range(1,len(fr)):
    t0,s0,m0=fr[i-1]; t1,s1,m1=fr[i]
    if m0 in (0x7b,0x6b) or m1 in (0x7b,0x6b): continue
    if t1-t0<0.005 and s1==s0+1:
        pairs+=1
        if any(s==s1 and m in(0x7b,0x6b) and t>t1 for t,s,m in fr[i+1:i+40]): re2+=1
print("pairs=%d second-retransmitted=%d"%(pairs,re2))
