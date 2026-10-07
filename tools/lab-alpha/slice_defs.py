import subprocess, sys, re, os
pod, logf, outdir = sys.argv[1:4]
mods = sys.argv[4:]
r=subprocess.run(['kubectl','-n','ovmx-lab','exec',pod,'--','sh','-c',"tr -d '\\000\\r' < %s" % logf],capture_output=True,timeout=600)
log=r.stdout.decode('utf-8','replace')
os.makedirs(outdir,exist_ok=True)
for m in mods:
    sm, em = "@@S"+m, "@@E"+m
    blocks=re.findall(r'^[^\n$]*%s\s*$(.*?)^[^\n$]*%s\s*$' % (re.escape(sm), re.escape(em)), log, re.S|re.M)
    body=max(blocks,key=len) if blocks else ''
    eq=[l for l in body.splitlines() if l.startswith('$EQU')]
    open(os.path.join(outdir,m+'.txt'),'w').write('\n'.join(eq)+('\n' if eq else ''))
    print('%s: %d' % (m,len(eq)))
