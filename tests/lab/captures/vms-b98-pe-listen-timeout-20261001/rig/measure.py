#!/usr/bin/env python3
# measure.py <pcap> <trial.out> <survivor.tlog> <victim-mac-hex> : per trial, silence before close
import sys, struct, re
pc, tr, tl, vm = sys.argv[1:5]
frames=[]
f=open(pc,'rb'); gh=f.read(24); ns=struct.unpack('<I',gh[:4])[0]==0xa1b23c4d
while True:
    h=f.read(16)
    if len(h)<16: break
    s,us,cl,ol=struct.unpack('<IIII',h); d=f.read(cl)
    if d[12:14]==b'\x60\x07': frames.append((s+us/(1e9 if ns else 1e6), d[6:12].hex(), d[0:6].hex(), d[14:]))
stops={}; conts={}
for l in open(tr):
    m=re.match(r'(T\d+) (STOP|CONT) ([\d.]+)',l)
    if m: (stops if m.group(2)=='STOP' else conts)[m.group(1)]=float(m.group(3))
closes=[]
for l in open(tl, errors='replace'):
    if 'Closed Virtual Circuit' in l or 'lost connection to system' in l:
        closes.append((float(l.split()[0]), l.strip()[15:80]))
for k in sorted(stops, key=lambda x:int(x[1:])):
    t0=stops[k]; t1=conts.get(k, t0+1e9)
    last=max([t for t,s,dd,p in frames if s==vm and t<=t0] or [0])
    c=[x for x in closes if t0<x[0]<t1+2]
    first=c[0][0] if c else None
    # survivor's directed frames to the victim during the stall
    dirs=['%.2f:%02x'%(t-last,p[16] if len(p)>16 else 0) for t,s,dd,p in frames if dd==vm and t0<t<(first or t1)]
    print('%s stop@%.3f last_rx=%.3f before stop, close=%s  SILENCE->CLOSE=%s  dir=%s' % (k, t0, t0-last,
          ('%.3f'%(first-t0)) if first else 'none', ('%.3f'%(first-last)) if first else '-', ' '.join(dirs[:8])))
