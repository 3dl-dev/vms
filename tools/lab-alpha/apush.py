#!/usr/bin/env python3
# usage: apush.py LOCALFILE REMOTENAME   -> writes the file on the Alpha lab via DCL OPEN/WRITE
import subprocess, sys, base64, time
src, rname = sys.argv[1], sys.argv[2]
import os; pod=os.environ.get('POD','corpusalpha-1'); node=os.environ.get('NODE','alpha1'); fifo='/lab/k8s-labs/%s/%s/logs/%s.log.in'%(pod,node,node)
def send(line):
    b=base64.b64encode(line.encode()).decode()
    subprocess.run(['kubectl','-n','ovmx-lab','exec',pod,'--','sh','-c',"{ echo %s | base64 -d; echo; } > %s"%(b,fifo)],capture_output=True)
    time.sleep(0.6)
send('OPEN/WRITE FW %s' % rname)
for ln in open(src).read().splitlines():
    q=ln.replace('"','""')
    send('WRITE FW "%s"' % q)
send('CLOSE FW')
time.sleep(2)
