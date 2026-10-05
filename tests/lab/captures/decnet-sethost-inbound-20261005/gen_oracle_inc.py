#!/usr/bin/env python3
"""Regenerate tests/vmsdecnet/cterm_host_oracle.inc from the two real-VAX-host
CTERM captures (lab tool, rd vms-a70). Run from anywhere."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '../../../..'))
sys.argv = ['x', 'x']
exec(open(os.path.join(HERE, 'nspdump.py')).read().split('t0=None')[0])
def segs(path):
    out=[]
    for ts,fr in frames(path):
        r=nsp(fr)
        if not r: continue
        s,d,n=r
        if n[0] in (0x00,0x20,0x40,0x60):
            j=5
            while j+1<len(n) and n[j+1]&0x80: j+=2
            j+=2
            out.append((s,d,n[j:]))
    return out
def carr(name,b):
    h=', '.join('0x%02x'%x for x in b)
    lines=[]; cur='    '
    for tok in h.split(', '):
        if len(cur)+len(tok)+2>80: lines.append(cur.rstrip()); cur='    '
        cur+=tok+', '
    lines.append(cur.rstrip().rstrip(','))
    return 'static const uint8_t %s[] = {\n%s\n};'%(name,'\n'.join(lines))
import itertools
o=segs(os.path.join(REPO,'docs/oracle/vax-sethost-cterm.pcap'))
l=segs(os.path.join(REPO,'tests/lab/captures/decnet-sethost-dcl-20260911/sethost-live-dcl.pcap'))

out=[]
out.append('/* GENERATED from the two real-VAX-host CTERM captures by the lab tool in')
out.append(' * tests/lab/captures/decnet-sethost-inbound-20261005/nspdump.py -- every NSP')
out.append(' * data-segment payload, in wire order. Do not edit by hand.')
out.append(' *   ORACLE_O: docs/oracle/vax-sethost-cterm.pcap  (host VAX2 1.2, client VAX1 1.1;')
out.append(' *             session 1 = idle Username: timeout, session 2 = login/DCL/logout)')
out.append(' *   ORACLE_L: tests/lab/captures/decnet-sethost-dcl-20260911/sethost-live-dcl.pcap')
out.append(' *             (host VAX1 1.1, client OVMX 1.42)')
out.append(' * host = 1 when the segment was sent by the CTERM HOST (the SET HOST target). */')
for nm,ss,hostaddr in (('o',o,1026),('l',l,1025)):
    for i,(s,d,p) in enumerate(ss):
        out.append(carr('k_%s%02d'%(nm,i),p))
    out.append('static const struct cth_oracle_seg k_oracle_%s[] = {'%nm)
    for i,(s,d,p) in enumerate(ss):
        out.append('    { %d, k_%s%02d, sizeof k_%s%02d },'%(1 if s==hostaddr else 0,nm,i,nm,i))
    out.append('};')
open(os.path.join(REPO,'tests/vmsdecnet/cterm_host_oracle.inc'),'w').write('\n'.join(out)+'\n')
