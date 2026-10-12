#!/usr/bin/env python3
"""dlm_seq_check.py <pcap> -- rd vms-64b. Every OVMX DLM frame (cat 0x02/0x82) must carry
body[12:14] = 0x0001 and body[14:16] = the N the real VAXes last asserted; lists the
transition opens (op 0x07/0x08/0x09) with their [92]/[100:102]/[102:104] cells."""
import struct,sys,collections
d=open(sys.argv[1],'rb').read(); o=24; t0=None
lastvax=None; bad=0; ov=collections.Counter(); vx=collections.Counter(); opens=[]
while o+16<=len(d):
    ts,tu,cl,_=struct.unpack('<IIII',d[o:o+16]); o+=16; f=d[o:o+cl]; o+=cl
    t=ts+tu/1e6; t0=t0 or t
    if len(f)!=204 or f[12:14]!=b'\x60\x07' or f[55]!=2: continue
    b=f[72:]; src=f[6:12].hex()[-4:]; isov=f[6:12].hex().startswith('525400')
    if b[8]==1 and b[9] in (7,8,9):
        opens.append('%8.2f %s>%s op%02x [92]=%d [100]=%d [102]=%d rebuild=%d'%(t-t0,src,f[0:6].hex()[-4:],b[9],b[92],b[100]|b[101]<<8,b[102]|b[103]<<8,b[24]))
    if b[8] not in (2,0x82): continue
    one=b[12]|b[13]<<8; n=b[14]|b[15]<<8
    if isov:
        ov[(one,n)]+=1
        if one!=1 or n==0 or (lastvax is not None and n!=lastvax):
            bad+=1
            if bad<=10: print('MISMATCH %8.2f %s %02x/%02x one=%d n=%d lastvax=%s'%(t-t0,src,b[8],b[9],one,n,lastvax))
    else:
        if one==1: lastvax=n; vx[n]+=1
print('OVMX DLM (one,N):',dict(ov)); print('VAX N:',dict(vx)); print('mismatches:',bad)
print('\n'.join(opens))
sys.exit(1 if bad else 0)
