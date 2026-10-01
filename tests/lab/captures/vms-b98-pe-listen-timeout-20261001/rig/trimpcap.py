#!/usr/bin/env python3
# trimpcap.py <in.pcap> <out.pcap> <trials.out...> : keep frames within [STOP-5, CONT+15] of every trial
import sys, struct, re
win=[]
for tf in sys.argv[3:]:
    st={}
    for l in open(tf):
        m=re.match(r'(T\d+) (STOP|CONT) ([\d.]+)',l)
        if m: st.setdefault(m.group(1),{})[m.group(2)]=float(m.group(3))
    for k,v in st.items():
        if 'STOP' in v and 'CONT' in v: win.append((v['STOP']-5, v['CONT']+15))
f=open(sys.argv[1],'rb'); o=open(sys.argv[2],'wb'); o.write(f.read(24)); n=0
while True:
    h=f.read(16)
    if len(h)<16: break
    s,us,cl,ol=struct.unpack('<IIII',h); d=f.read(cl); t=s+us/1e6
    if any(a<=t<=b for a,b in win): o.write(h); o.write(d); n+=1
print(n)
