import struct,sys,collections
c=collections.Counter()
for p in sys.argv[1:]:
    try: d=open(p,'rb').read()
    except: continue
    off=24; reqs={}
    while off+16<=len(d):
        ts,tus,cl,_=struct.unpack("<IIII",d[off:off+16]); off+=16; f=d[off:off+cl]; off+=cl
        if f[12:14]!=b"\x60\x07": continue
        s=f[14:]
        if len(s)<58+30 or struct.unpack("<H",s[46:48])[0]!=10: continue
        b=bytes(s[58:58+132]); src=f[6:12].hex(); dst=f[0:6].hex()
        if b[8]==0x06 and b[9]==0: reqs[(src,dst,b[4:8])]=b
        elif b[8]==0x86 and b[9]==0:
            q=reqs.get((dst,src,b[4:8]))
            if q is not None: c[(q[26],b[24])]+=1
for k,v in sorted(c.items()): print('req[26]=%d -> rsp[24]=%d : %d'%(k[0],k[1],v))
