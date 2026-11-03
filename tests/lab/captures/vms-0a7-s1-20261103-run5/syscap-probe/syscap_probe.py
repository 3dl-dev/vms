#!/usr/bin/env python3
"""rd vms-5b9 black-box probe: which CONFIGURATION SYSCAP bit makes the real VAX
V7.3 FAL send DIRECTORY LIST dates in the DAP 7 binary form (DATMENU bits 7/8)
rather than the DAP 5.6 A-18 text form? OVMXA (an experiment image whose RMS
client ORs in the SYSCAP bits listed in the process logical S1R5_SYSCAP) runs
DIRECTORY/FULL against VAXC once per candidate set; the listing's hundredths
(and the wire) show which form the VAX sent.
usage: syscap_probe.py <outdir> <ovmx-disk>   env W, REL, VAX_PW, OVMX_PW"""
import os, re, sys, time, subprocess
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
OUT, IMG = sys.argv[1], sys.argv[2]
import fv as F
from node import Node
VAXONLY = [0, 2, 4, 6, 7, 8, 10, 11, 12, 13, 14, 17, 21, 22, 23, 29, 30, 31, 43, 44, 51, 53, 56, 57, 58,
           61, 62, 63, 64, 65, 66, 67, 68, 69]
def main():
    relay = subprocess.Popen([sys.executable, os.path.join(HERE, 'udp2mcast.py'), '230.0.0.93', '12493',
                              '41093', '41094'], stdout=open(os.path.join(OUT, 'relay.log'), 'a'), stderr=subprocess.STDOUT)
    v = F.Vax(); a = None
    try:
        v.boot(); v.login(); F.ev('VAXC logged in'); v.license(); F.ev('VAXC licensed')
        v.cmd('@SYS$STARTUP:STARTNET', t=300)
        for _ in range(60):
            if re.search(r'State\s*=\s*on', v.cmd('MCR NCP SHOW EXECUTOR')): break
            time.sleep(5)
        F.ev('VAXC DECnet on')
        a = Node('A', IMG, '52:54:00:06:a5:0b', os.path.join(OUT, 'ovmxa-console.log'),
                 mcast='230.0.0.93:12493', pcap=os.path.join(OUT, 'ovmxa.pcap'))
        a.boot_login(pw=F.OVMX_PW); F.ev('OVMXA logged in')
        a.cmd('NCP :== $SYS$SYSTEM:NCP.EXE', t=30)
        v.create('SYS$LOGIN:PROBE.TXT', ['probe file'])
        for _ in range(30):
            if re.search(r'State\s*=\s*on', a.cmd('NCP SHOW EXECUTOR', t=60)): break
            time.sleep(2)
        S = f'VAXC"SYSTEM {F.VAX_PW}"::SYS$LOGIN:PROBE.TXT'
        def probe(tag, bits):
            if bits: a.cmd('DEFINE/PROCESS S1R5_SYSCAP "' + ','.join(map(str, bits)) + '"', t=30)
            else: a.cmd('DEASSIGN/PROCESS S1R5_SYSCAP', t=30)
            F.ev('probe', tag, 'bits', bits)
            out = a.cmd('DIRECTORY/FULL ' + S, t=120)
            m = re.search(r'Created:\s+(\S+ [0-9:.]+)', out)
            F.ev('probe', tag, 'Created', m.group(1) if m else '<none>', 'hundredths' if m and not m.group(1).endswith('.00') else 'TEXT-FORM')
            return bool(m) and not m.group(1).endswith('.00')
        probe('base', [])
        hits = [b for b in VAXONLY if probe(f'bit{b}', [b])]
        F.ev('RESULT: bits giving binary times:', hits)
        probe('base-again', [])
    finally:
        if a: a.close()
        subprocess.run(['docker', 'rm', '-f', 'vms0a7s1r5x-vaxc'], capture_output=True)
        relay.terminate()
main()
