#!/usr/bin/env python3
"""rd vms-05d lab bench: OVMXA (x86 OVMX V0.7-13, KVM) and VAXC (real VMS V7.3,
SIMH, licensed scratch copy) on one L2 segment; SET HOST both ways."""
import sys, time, os, pexpect
sys.path.insert(0, '/tmp/dnlab')
from node import Node
OUT = '/tmp/vms05d-work/byol'
VFIFO = OUT + '/logs/vaxc.log.in'
VLOG = OUT + '/logs/vaxc.log'

def vsend(s):
    with open(VFIFO, 'w') as f:
        f.write(s + '\r')

def vwait(pat, start, t=120):
    import re
    end = time.time() + t
    while time.time() < end:
        with open(VLOG, 'rb') as f:
            f.seek(start)
            txt = f.read().decode('latin1').replace('\r', '').replace('\0', '')
        if re.search(pat, txt, re.M):
            return txt
        time.sleep(1)
    raise RuntimeError('VAXC timeout waiting for %r' % pat)

a = Node('A', OUT + '/A-vaxc.img', '52:54:00:00:00:0a', OUT + '/ovmxa-console.log',
         mcast='230.0.0.9:12399', pcap=OUT + '/ovmxa.pcap')
res = []
try:
    a.boot_login()
    a.send('')
    time.sleep(20)
    a.cmd('NCP :== $SYS$SYSTEM:NCP.EXE', t=30); a.cmd('NCP SHOW KNOWN NODES', t=60)
    try:
        a.p.expect(pexpect.TIMEOUT, timeout=8)   # drain console chatter before SET HOST
    except Exception:
        pass
    # --- direction 1: OVMXA -> VAXC
    a.send('SET HOST VAXC')
    i = a.p.expect([r'Username: ', r'%[A-Z]+-[EFW]-[A-Z]+, [^\r\n]*', pexpect.TIMEOUT], timeout=120)
    if i != 0:
        res.append(('OVMXA->VAXC', 'FAIL', a.p.after if i == 1 else 'TIMEOUT'))
    else:
        time.sleep(1); a.send('SYSTEM')
        a.p.expect('Password:', timeout=60); time.sleep(1); a.send('OVMXCLUSTER1')
        j = a.p.expect([r'\$ ', r'%REM-S-END'], timeout=180)
        if j == 1:
            raise RuntimeError('remote login refused: ' + a.p.before.decode('latin1')[-200:])
        time.sleep(3)
        a.send('WRITE SYS$OUTPUT "WHO=",F$GETSYI("NODENAME")')
        a.p.expect(r'WHO=[A-Z0-9]+', timeout=120); who = a.p.after.decode('latin1')
        time.sleep(2)
        a.send('SHOW TERMINAL')
        a.p.expect(r'\$ ', timeout=120)
        time.sleep(2); a.send('LOGOUT')
        a.p.expect(r'%REM-S-END[^\r\n]*|logged out[^\r\n]*', timeout=120); end = a.p.after.decode('latin1')
        res.append(('OVMXA->VAXC', 'OK', who, end))
    print(res[-1], flush=True)
    time.sleep(5)
    # --- direction 2: VAXC -> OVMXA
    st = os.path.getsize(VLOG)
    vsend('SET HOST OVMXA')
    txt = vwait(r'Username: ?$|%[A-Z]+-[EFW]-[A-Z]+,', st, 120)
    if 'Username' not in txt.split('SET HOST OVMXA', 1)[-1]:
        res.append(('VAXC->OVMXA', 'FAIL', txt[-400:]))
    else:
        vsend('SYSTEM'); vwait(r'Password: ?$', st, 60); time.sleep(1); vsend('MANAGER')
        vwait(r'Welcome to OpenVMX|\$ $', st, 180); time.sleep(5)
        st2 = os.path.getsize(VLOG)
        vsend('WRITE SYS$OUTPUT "WHO=",F$GETSYI("NODENAME")')
        txt = vwait(r'^WHO=\S+', st2, 120)
        time.sleep(2); vsend('SHOW TERMINAL'); time.sleep(6)
        vsend('LOGOUT')
        txt = vwait(r'%REM-S-END|logged out', st2, 120)
        res.append(('VAXC->OVMXA', 'OK', [l for l in txt.splitlines() if l.startswith('WHO=')][:1]))
    print(res[-1], flush=True)
    time.sleep(3)
    a.cmd('NCP :== $SYS$SYSTEM:NCP.EXE', t=30); a.cmd('NCP SHOW KNOWN NODES', t=60)
finally:
    a.close()
print('RESULTS', res)
