import struct,sys,collections,hashlib
c=collections.Counter(); seen=set(); miss=[]
for p in sys.argv[1:]:
    d=open(p,'rb').read(); off=24; fr=[]
    while off+16<=len(d):
        ts,tus,cl,_=struct.unpack("<IIII",d[off:off+16]); off+=16; f=d[off:off+cl]; off+=cl
        if f[12:14]!=b"\x60\x07": continue
        s=f[14:]
        if len(s)<58+132 or struct.unpack("<H",s[46:48])[0]!=10: continue
        if f[6:9].hex()=='525400' or f[0:3].hex()=='525400': continue
        fr.append((ts+tus/1e6,f[6:12].hex(),f[0:6].hex(),bytes(s[58:58+132])))
    last0e={}; reqs={}
    for (t,s_,d_,b) in fr:
        nm=b[48:48+b[47]] if b[8]==2 else None
        if b[8]==2 and b[9]==0x0e: last0e[(s_,d_,nm)]=t
        if b[8]==2 and b[9]==0x14: last0e.pop((d_,s_,nm),None)
        if b[8]==2 and b[9]==0x0f:
            t0=last0e.get((s_,d_,nm))
            reqs[(s_,d_,b[0:2])]=(b, t0 is not None)
        elif b[8]==0x82 and b[9]==0x15:
            r=reqs.pop((d_,s_,b[2:4]),None)
            if not r: continue
            q,pending=r
            h=hashlib.sha1(q+b).digest()
            if h in seen: continue
            seen.add(h)
            exp=bytearray(q); exp[8]=0x82; exp[9]=0x15; exp[32:36]=b'\x01\x00\xfa\x00'
            if pending: exp[28:32]=b'\0\0\0\0'
            ok=bytes(exp[4:])==b[4:]
            c[(pending,ok)]+=1
            if not ok and len(miss)<8: miss.append((p.split('/')[-1],pending,q[24:40].hex(),b[24:40].hex()))
print(c); [print(m) for m in miss]
