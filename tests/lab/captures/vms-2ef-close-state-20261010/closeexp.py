import struct,sys
pc,mk=sys.argv[1],sys.argv[2]
marks=[]
for l in open(mk):
    t,tag,node,*cmd=l.split(); marks.append((float(t),tag,node,' '.join(cmd)))
def step(t):
    s='pre'
    for m in marks:
        if t>=m[0]: s=m[1]+' '+m[2]
    return s
d=open(pc,'rb').read(); off=24; reqs={}
name={'0104':'VAX1','0204':'VAX2'}
while off+16<=len(d):
    ts,tus,cl,_=struct.unpack("<IIII",d[off:off+16]); off+=16; f=d[off:off+cl]; off+=cl
    if f[12:14]!=b"\x60\x07": continue
    s=f[14:]; src=f[6:12].hex()[-4:]; dst=f[0:6].hex()[-4:]
    if len(s)<58+30 or struct.unpack("<H",s[46:48])[0]!=10: continue
    b=bytes(s[58:58+132]); t=ts+tus/1e6
    if b[8]==0x06 and b[9]==0: reqs[(src,dst,b[4:8])]=b
    elif b[8]==0x86 and b[9]==0:
        q=reqs.get((dst,src,b[4:8]))
        if q is not None: print('%-12s req %s->%s [26]=%d [28:30]=%s | rsp[24]=%d'%(step(t),name.get(dst,dst),name.get(src,src),q[26],q[28:30].hex(),b[24]))
