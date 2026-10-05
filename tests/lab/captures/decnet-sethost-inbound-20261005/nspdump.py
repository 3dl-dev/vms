#!/usr/bin/env python3
"""Dump NSP data-segment payloads (CTERM) from a DECnet pcap. Lab instrument.
usage: nspdump.py FILE.pcap [a]   (a = also print each payload as a Python bytes literal)"""
import struct, sys
def frames(path):
    d=open(path,'rb').read()
    magic=struct.unpack('<I',d[:4])[0]
    e='<' if magic in (0xa1b2c3d4,0xa1b23c4d) else '>'
    off=24
    while off+16<=len(d):
        ts,tu,cl,ol=struct.unpack(e+'IIII',d[off:off+16]); off+=16
        yield ts+tu/1e6, d[off:off+cl]; off+=cl
def nsp(fr):
    if len(fr)<16 or fr[12:14]!=b'\x60\x03': return None
    L=struct.unpack('<H',fr[14:16])[0]; p=fr[16:16+L]
    i=0
    if p[0]&0x80: i+= p[0]&0x7f
    fl=p[i]
    if fl&1: return None # control
    if (fl&0x07)==0x06: src=p[i+11:i+17]; dst=p[i+3:i+9]; i+=21
    elif (fl&0x07)==0x02: dst=p[i+1:i+3]; src=p[i+3:i+5]; i+=6
    else: return None
    s=struct.unpack('<H',src[-2:])[0] if len(src)==6 else struct.unpack('<H',src)[0]
    dd=struct.unpack('<H',dst[-2:])[0] if len(dst)==6 else struct.unpack('<H',dst)[0]
    return s,dd,p[i:]
def addr(a): return "%d.%d"%(a>>10,a&1023)
t0=None
for ts,fr in frames(sys.argv[1]):
    r=nsp(fr)
    if not r: continue
    s,d,n=r
    if t0 is None: t0=ts
    mf=n[0]
    names={0x00:'DATAmid',0x20:'DATAbom',0x40:'DATAeom',0x60:'DATA',0x10:'INT',0x30:'LS',0x04:'ACK',0x14:'OACK',0x08:'CI',0x18:'RCI',0x28:'CC',0x38:'DI',0x48:'DC',0x58:'NOP'}
    nm=names.get(mf,'%02x'%mf)
    if mf in (0x00,0x20,0x40,0x60):
        j=5
        while j+1<len(n) and n[j+1]&0x80: j+=2
        segnum=struct.unpack('<H',n[j:j+2])[0]&0xfff; j+=2
        pl=n[j:]
        print("%8.3f %s->%s %s seg%d len%d: %s"%(ts-t0,addr(s),addr(d),nm,segnum,len(pl),pl.hex(' ')))
        if len(sys.argv)>2: print("            %r"%bytes(pl))
    elif mf==0x10 or mf==0x30:
        print("%8.3f %s->%s %s: %s"%(ts-t0,addr(s),addr(d),nm,n.hex(' ')))
    elif mf in (0x08,0x18,0x28,0x38,0x48):
        print("%8.3f %s->%s %s: %s"%(ts-t0,addr(s),addr(d),nm,n.hex(' ')))
