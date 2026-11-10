#!/usr/bin/env python3
"""rd vms-0a7 S3 diagnostic: what happens to NETACP across an inbound SET HOST.
Same bench as s3_bench.py (imports it), but OVMXA boots with kernel loglevel=7 and
print-fatal-signals=1 so a fatal signal in any task reaches the console, and the
OVMXA console samples SHOW SYSTEM / NETACP.LOG at each stage of the VAX's SET HOST:
connected (at Username:), after a WRONG password, and after a right one.

usage: probe_sethost.py <outdir> <ovmx-disk-image>   (env as s3_bench.py)
"""
import os, re, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.argv = sys.argv[:3]
import node
import s3_bench as B

# boot OVMXA with fatal signals reported on its console
_spawn = node.pexpect.spawn
def spawn(cmd, args, **k):
    args = [('console=ttyS0 loglevel=7 print-fatal-signals=1' if x == 'console=ttyS0 loglevel=3 quiet' else x)
            for x in args]
    return _spawn(cmd, args, **k)
node.pexpect.spawn = spawn

def main():
    ev = B.ev
    relay = subprocess.Popen([sys.executable, os.path.join(HERE, 'udp2mcast.py'), B.MGRP, B.MPORT,
                              B.SIMH_PORT, B.RELAY_PORT], stdout=open(os.path.join(B.OUT, 'relay.log'), 'a'),
                             stderr=subprocess.STDOUT)
    v = B.Vax(); a = None
    try:
        v.boot(); v.login(); ev('VAXC logged in'); v.license(); ev('VAXC licensed')
        v.cmd('@SYS$STARTUP:STARTNET', t=300)
        for _ in range(60):
            if re.search(r'State\s*=\s*on', v.cmd('MCR NCP SHOW EXECUTOR')):
                break
            time.sleep(5)
        ev('VAXC DECnet on')
        a = node.Node('A', B.IMG, '52:54:00:06:a5:0a', os.path.join(B.OUT, 'ovmxa-console.log'),
                      mcast=f'{B.MGRP}:{B.MPORT}', pcap=os.path.join(B.OUT, 'ovmxa.pcap'))
        a.boot_login(pw=B.OVMX_PW); ev('OVMXA logged in')
        acmd = lambda c, t=60: a.cmd(c, t=t, until=B.OPROMPT)
        def sample(tag):
            ev('sample', tag)
            acmd('SHOW SYSTEM'); acmd('TYPE SYS$MANAGER:NETACP.LOG')
        sample('P0 before')
        FG = os.environ.get('RESTART_NETACP') in ('fg', 'fglog')
        if FG:
            # NETACP in the FOREGROUND of the console process: its SYS$OUTPUT and
            # SYS$ERROR are the console, and DCL reports how the image ended.
            pid = re.search(r'([0-9A-F]{8}) NETACP', acmd('SHOW SYSTEM')).group(1)
            ev('P0f STOP NETACP', pid)
            acmd(f'STOP/IDENTIFICATION={pid}')
            time.sleep(3)
            if os.environ.get('RESTART_NETACP') == 'fglog':
                # the detached NETACP's NETACP.LOG sink, switched on in the foreground:
                # SYS$OUTPUT / SYS$ERROR name a file for this one image
                acmd('DEFINE/USER_MODE SYS$OUTPUT SYS$MANAGER:NETACPFG.LOG')
                acmd('DEFINE/USER_MODE SYS$ERROR SYS$MANAGER:NETACPFG.LOG')
            a.send('RUN SYS$SYSTEM:NETACP.EXE')
            time.sleep(8)
            sample = lambda tag: ev('sample', tag, '(console busy: NETACP in the foreground)')
        elif os.environ.get('RESTART_NETACP'):
            # the pre-S3 way of starting it: DCL RUN/DETACHED (STARTNET before S3)
            pid = re.search(r'([0-9A-F]{8}) NETACP', acmd('SHOW SYSTEM')).group(1)
            ev('P0r STOP NETACP', pid)
            acmd(f'STOP/IDENTIFICATION={pid}')
            time.sleep(3)
            acmd('SHOW SYSTEM')
            lg = os.environ.get('NETACP_OUT', 'SYS$MANAGER:NETACP.LOG')   # NL: = no RMS log sink
            acmd(f'RUN/DETACHED/PROCESS_NAME=NETACP/INPUT=NL:/OUTPUT={lg}'
                 f'/ERROR={lg} SYS$SYSTEM:NETACP.EXE')
            time.sleep(5)
            sample('P0r after RUN/DETACHED')
        time.sleep(15)          # adjacency up
        if os.environ.get('PROBE_COPY'):
            # the sustained-transfer trigger instead of a login: the VAX pushes a
            # 2000-record file into OVMX's FAL and pulls it back
            O = f'OVMXA"SYSTEM {B.OVMX_PW}"::'
            v.create('SYS$SCRATCH:MKBIG.COM', ["$ OPEN/WRITE F SYS$LOGIN:BIG.TXT", "$ I = 0",
                     "$ L: I = I + 1", "$ N = F$FAO(\"!4ZL\",I)",
                     "$ WRITE F \"record ''N' of the S3 probe copy, abcdefghijklmnopqrstuvwxyz0123456789\"",
                     "$ IF I .LT. 2000 THEN GOTO L", "$ CLOSE F"])
            v.cmd('@SYS$SCRATCH:MKBIG.COM', t=300)
            def vst(tag, c, t=600):
                ev('P', tag, 'start'); v.cmd(c, t=t)
                st = v.cmd('SHOW SYMBOL $STATUS'); m = re.search(r'%X[0-9A-F]+', st)
                ev('P', tag, 'done', m.group(0) if m else '?')
            vst('C0', f'DIRECTORY {O}')
            ev('P C1 start (OVMXA samples SHOW SYSTEM)')
            v.p.send(f'COPY SYS$LOGIN:BIG.TXT {O}BIGV.TXT\r')
            if not FG:
                for n in range(3):
                    time.sleep(3); acmd('SHOW SYSTEM')
            v.p.expect(B.PROMPT, timeout=600)
            st = v.cmd('SHOW SYMBOL $STATUS'); m = re.search(r'%X[0-9A-F]+', st)
            ev('P C1 done', m.group(0) if m else '?')
            vst('C2', f'COPY {O}BIGV.TXT SYS$LOGIN:BIGVB.TXT')
            vst('C3', 'DIFFERENCES/MAXIMUM_DIFFERENCES=3 SYS$LOGIN:BIG.TXT SYS$LOGIN:BIGVB.TXT')
            for f in ('BIG.TXT', 'BIGVB.TXT'):
                v.cmd(f'CHECKSUM SYS$LOGIN:{f}', t=300); v.cmd('SHOW SYMBOL CHECKSUM$CHECKSUM')
            v.cmd('DIRECTORY/SIZE=ALL SYS$LOGIN:BIG.TXT,BIGVB.TXT')
            vst('C4', f'DIRECTORY/SIZE {O}BIGV.TXT')
            if not FG:
                acmd('DIRECTORY/SIZE SYS$LOGIN:BIGV.TXT'); acmd('SHOW SYSTEM')
                acmd('TYPE SYS$MANAGER:NETACP.LOG')
                # then the inbound login, with the OVMXA console free to look at NETACP
                ev('P5 VAXC SET HOST OVMXA')
                v.p.send('SET HOST OVMXA\r'); v.p.expect('Username:', timeout=120)
                v.p.send('SYSTEM\r'); v.p.expect('Password:', timeout=60); time.sleep(0.5)
                v.p.send(B.OVMX_PW + '\r')
                k = v.p.expect(['REM-S-END', B.PROMPT, node.pexpect.TIMEOUT], timeout=60)
                ev('P5 after login: expect index', k, '(0 ended, 1 prompt, 2 no prompt in 60 s)')
                acmd('SHOW SYSTEM'); acmd('TYPE SYS$MANAGER:NETACP.LOG')
                if k == 1:
                    v.cmd('SHOW SYSTEM'); v.p.send('LOGOUT\r'); v.p.expect(B.PROMPT, timeout=120)
                ev('DONE')
                return
            k = a.p.expect([B.OPROMPT, node.pexpect.TIMEOUT], timeout=10)
            ev('PC NETACP foreground image ended' if k == 0 else 'PC NETACP still running')
            if k == 1:
                a.p.send('\x19'); time.sleep(3); a.p.send('\r'); a.p.expect(B.OPROMPT, timeout=60)
            acmd('SHOW SYMBOL $STATUS'); acmd('SHOW SYSTEM')
            acmd('DIRECTORY/SIZE SYS$LOGIN:BIGV.TXT')
            ev('DONE')
            return
        ev('P1 VAXC SET HOST OVMXA')
        v.p.send('SET HOST OVMXA\r'); v.p.expect('Username:', timeout=120)
        sample('P1 at Username:')
        v.p.send('SYSTEM\r'); v.p.expect('Password:', timeout=60); time.sleep(0.5)
        v.p.send(B.BAD_PW + '\r')
        k = v.p.expect(['Username:', 'REM-S-END', B.PROMPT], timeout=60)
        ev('P2 after wrong password: expect index', k)
        sample('P2 after wrong password')
        if k == 0:
            v.p.send('SYSTEM\r'); v.p.expect('Password:', timeout=60); time.sleep(0.5)
            v.p.send(B.OVMX_PW + '\r')
            k = v.p.expect(['REM-S-END', B.PROMPT], timeout=120)
            ev('P3 after right password: expect index', k, '(0 = session ended)')
            time.sleep(2)
            sample('P3 after right password')
            if k == 1:
                v.cmd('SHOW SYSTEM'); v.p.send('LOGOUT\r'); v.p.expect(B.PROMPT, timeout=120)
        else:
            v.p.expect(B.PROMPT, timeout=120)
        time.sleep(3)
        if FG:
            k = a.p.expect([B.OPROMPT, node.pexpect.TIMEOUT], timeout=60)
            ev('P4f NETACP foreground image ended' if k == 0 else 'P4f NETACP still running')
            if k == 1:
                a.p.send('\x19'); a.p.expect(B.OPROMPT, timeout=60)
            acmd('SHOW SYMBOL $STATUS')
            acmd('TYPE SYS$MANAGER:NETACPFG.LOG')
            sample = lambda tag: (acmd('SHOW SYSTEM'), acmd('TYPE SYS$MANAGER:NETACP.LOG'))
        sample('P4 end')
        ev('DONE')
    finally:
        if a:
            a.close()
        subprocess.run(['docker', 'rm', '-f', B.CTR], capture_output=True)
        relay.terminate()

if __name__ == '__main__':
    main()
