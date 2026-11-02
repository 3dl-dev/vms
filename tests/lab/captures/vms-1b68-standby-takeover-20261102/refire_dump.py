import struct,sys
d=open(sys.argv[1],'rb').read(); i=24; n=0
while i+16<=len(d):
    s,u,cap,_=struct.unpack('<IIII',d[i:i+16]); f=d[i+16:i+16+cap]; i+=16+cap; n+=1
    if len(f)<200 or f[12:14]!=b'\x60\x07': continue
    if (f[80]&0x7f)!=0x02: continue
    rl=f[119]; rn=f[120:120+rl] if 1<=rl<=31 else b''
    if len(sys.argv)>2:
        if not (int(sys.argv[2])<=n<=int(sys.argv[3])): continue
    elif rn not in (b"WKL3",b"EVAC$WORKLOAD"): continue
    b=lambda k:f[72+k]
    import time
    print(f"f{n} {time.strftime('%H:%M:%S',time.gmtime(s))} src={f[6:12].hex()} dst={f[0:6].hex()} cat={f[80]:02x} op={f[81]:02x} res={rn.decode()} b28={b(28):02x} b30={b(30):02x} b31={b(31):02x} b108={b(108):02x} b110={b(110):02x} b20={struct.unpack('<I',f[92:96])[0]:08x} b24={struct.unpack('<I',f[96:100])[0]:08x}")
