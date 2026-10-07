#!/usr/bin/env python3
import subprocess, sys, base64, time, os, re
pod, fifo, logf, lib, outdir = sys.argv[1:6]
mods = sys.argv[6:]
def k(cmd, inp=None, timeout=120):
    return subprocess.run(['kubectl','-n','ovmx-lab','exec',pod,'--','sh','-c',cmd],capture_output=True,text=True,timeout=timeout)
def send(line):
    b=base64.b64encode(line.encode()).decode()
    k("{ echo %s | base64 -d; echo; } > %s" % (b, fifo))
def count(marker):
    r=k("tr -d '\\000\\r' < %s | grep '%s' | grep -vc WRITE" % (logf, marker))
    try: return int(r.stdout.strip() or 0)
    except: return 0
os.makedirs(outdir, exist_ok=True)
for m in mods:
    sm, em = "@@S"+m, "@@E"+m
    send("LIBRARY/MACRO/EXTRACT=$%s /OUTPUT=SYS$SCRATCH:X%s.MAR SYS$LIBRARY:%s.MLB" % (m, m, lib))
    time.sleep(3)
    send('WRITE SYS$OUTPUT "%s"' % sm)
    send("TYPE SYS$SCRATCH:X%s.MAR" % m)
    send('WRITE SYS$OUTPUT "%s"' % em)
    t0=time.time()
    while count(em)<1 and time.time()-t0<300:
        time.sleep(2)
    send("DELETE SYS$SCRATCH:X%s.MAR;*" % m)
    print(m, round(time.time()-t0), flush=True)
# slice
r=k("tr -d '\\000\\r' < %s" % logf, timeout=300)
log=r.stdout
for m in mods:
    sm, em = "@@S"+m, "@@E"+m
    mt=re.search(r'^[^\n$]*%s\s*$(.*?)^[^\n$]*%s\s*$' % (re.escape(sm), re.escape(em)), log, re.S|re.M)
    body = mt.group(1) if mt else ''
    eq=[l for l in body.splitlines() if l.startswith('$EQU')]
    open(os.path.join(outdir,m+'.txt'),'w').write('\n'.join(eq)+('\n' if eq else ''))
    print('%s: %d' % (m, len(eq)))
