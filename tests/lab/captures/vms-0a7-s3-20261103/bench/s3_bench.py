#!/usr/bin/env python3
"""rd vms-0a7 S3 live proof: NETACP.EXE as its own detached process (NCP SET EXECUTOR
STATE ON from STARTNET), inbound object-17 connects served by FAL.EXE that NETACP
$CREPRCs with SYS$NET, and the S1 RMS client path, against a real OpenVMS VAX V7.3.
Copied from the run-5 S1 bench (../vms-0a7-s1-20261103-run5/bench/fal_verbs.py).

What is new against run 5:
- (1) SHOW SYSTEM on OVMXA's console, TYPE SYS$MANAGER:NETACP.LOG, and SHOW SYSTEM from
  the VAX through SET HOST OVMXA (inbound CTERM served by NETACP).
- (4) SYS$SYSTEM:DECNETD.EXE is RENAMEd away for the WHOLE SYSTEM (a scratch disk) right
  after login, before any DECnet command; positive control SET HOST VAXC (on this branch
  still the DECNETD.EXE client) must then fail, and every other case runs without it.
  SHOW SYSTEM is sampled on OVMXA while the VAX pushes BIG.TXT into OVMX's FAL.
- (2) VAX -> OVMX: DIRECTORY OVMXA"..."::, COPY BIG.TXT into OVMX's FAL and back, compared
  on the VAX (DIFFERENCES, CHECKSUM).
- (3) OVMX -> VAX: DIRECTORY / TYPE / COPY VAXC"..."::, BIG.TXT round trip.

usage: s3_bench.py <outdir> <ovmx-disk-image>   (env REL, W, VAX_PW DN_PW OVMX_PW BAD_PW)
"""
import json, os, re, subprocess, sys, time
import pexpect
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from node import Node

W = os.environ.get('W', '/tmp/s3r1-lab')
OUT, IMG = sys.argv[1], sys.argv[2]
VAX_PW = os.environ['VAX_PW']
DN_PW = os.environ['DN_PW']
OVMX_PW = os.environ['OVMX_PW']
BAD_PW = os.environ['BAD_PW']
CTR = 's3r1-vaxc'
MGRP, MPORT, SIMH_PORT, RELAY_PORT = '230.0.0.95', '12495', '41095', '41096'
os.makedirs(OUT, exist_ok=True)
EVENTS = open(os.path.join(OUT, 'events.log'), 'a')

def ev(*a):
    s = time.strftime('%H:%M:%S ') + ' '.join(str(x) for x in a)
    print(s, flush=True); EVENTS.write(s + '\n'); EVENTS.flush()

PROMPT = r'\r\n[\r\x00]*\$ $'
# OVMXA's console prompt is CR LF CR NUL "$ "; a bare '\$ ' also matches "$ " inside
# output (NETACP.LOG names "$ SET HOST"), which desynchronised the first attempt.
OPROMPT = r'\n\r\x00\$ $'

class Vax:
    def __init__(self):
        import pexpect
        subprocess.run(['docker', 'rm', '-f', CTR], capture_output=True)
        self.log = open(os.path.join(OUT, 'vaxc-console.log'), 'ab')
        self.p = pexpect.spawn('docker', ['run', '--rm', '-it', '--name', CTR,
                               '--network', 'host', '--user', f'{os.getuid()}:{os.getgid()}',
                               '-v', f'{W}/vaxw:/w', '-w', '/w', '--entrypoint', '/usr/local/bin/vax',
                               '192.168.2.43:30500/ovmx-vaxlab:5', 'vax.ini'], timeout=60)
        self.p.logfile_read = self.log

    def slow(self, s):
        for ch in s:
            self.p.send(ch); time.sleep(0.15)
        self.p.send('\r')

    def boot(self):
        import pexpect
        booted = False
        end = time.time() + 900
        while time.time() < end:
            i = self.p.expect([r'>>>', r'ENTER DATE AND TIME', r'Username: ?$', pexpect.TIMEOUT], timeout=20)
            if i == 0 and not booted:
                time.sleep(1); self.slow('B DUA0'); booted = True
            elif i == 1:
                self.slow(time.strftime('%d-%b-%Y %H:%M').upper())
            elif i == 2:
                return
            elif i == 3 and booted:
                self.p.send('\r')
        raise RuntimeError('VAXC did not reach Username:')

    def login(self):
        # The boot match can land inside OPCOM's audit text ("Username:   SYSTEM");
        # a real prompt is a "Username: " with nothing after it for a while.
        import pexpect
        for _ in range(20):
            try:
                self.p.expect(r'.+', timeout=3)
                continue            # more output: not quiet at a prompt yet
            except pexpect.TIMEOUT:
                pass
            self.p.send('\r')
            try:
                self.p.expect(r'Username: $', timeout=20)
            except pexpect.TIMEOUT:
                continue
            try:
                self.p.expect(r'.+', timeout=2)
            except pexpect.TIMEOUT:
                break
        for attempt in range(3):
            self.p.send('SYSTEM\r')
            try:
                self.p.expect('Password:', timeout=60)
                break
            except pexpect.TIMEOUT:      # typed into STARTUP's tail: ask again
                self.p.send('\r'); time.sleep(5)
        time.sleep(1)
        self.p.send(VAX_PW + '\r'); self.p.expect(r'\$ $', timeout=180)

    def cmd(self, c, t=180):
        self.p.send(c + '\r'); self.p.expect(PROMPT, timeout=t)
        return self.p.before.decode('latin1', 'replace')

    def create(self, path, lines):
        self.p.send(f'CREATE {path}\r'); time.sleep(1)
        for l in lines:
            self.p.send(l + '\r'); time.sleep(0.3)
        self.p.send('\x1a'); self.p.expect(PROMPT, timeout=60)

    def license(self):
        paks = json.load(open(f'{W}/secret/paks3.json'))
        try:
            self.p.logfile_read = None
            self.log.write(b'\r\n[bench: LICENSE REGISTER of DVNETRTG, VAX-VMS, VAXCLUSTER typed here from the '
                           b'lab PAKs. The console log is paused; PAK contents are REDACTED ENTIRELY.]\r\n')
            for blk in paks:
                for k, line in enumerate(blk):
                    line = line[2:] if k == 0 and line.startswith('$ ') else line
                    self.p.send(line + '\r')
                    self.p.expect([r'_\$ $', PROMPT], timeout=60)
                    time.sleep(0.3)
        except Exception:
            raise RuntimeError('VAXC: LICENSE REGISTER did not complete (buffer suppressed)') from None
        self.p.logfile_read = self.log
        self.p.send('\r'); self.p.expect(r'\$ $', timeout=30)
        for prod in ('DVNETRTG', 'VAX-VMS', 'VAXCLUSTER'):
            self.cmd('LICENSE LOAD ' + prod)

def main():
    relay = subprocess.Popen([sys.executable, os.path.join(HERE, 'udp2mcast.py'), MGRP, MPORT,
                              SIMH_PORT, RELAY_PORT], stdout=open(os.path.join(OUT, 'relay.log'), 'a'),
                             stderr=subprocess.STDOUT)
    v = Vax(); a = None
    try:
        v.boot(); v.login(); ev('VAXC logged in'); v.license(); ev('VAXC licensed')
        v.cmd('@SYS$STARTUP:STARTNET', t=300)
        for _ in range(60):
            if re.search(r'State\s*=\s*on', v.cmd('MCR NCP SHOW EXECUTOR')):
                break
            time.sleep(5)
        else:
            raise RuntimeError('VAXC: DECnet executor never reached state on')
        ev('VAXC DECnet on')
        v.cmd('SET DEFAULT SYS$SYSTEM')
        v.cmd(f'MCR AUTHORIZE ADD DNTEST/PASSWORD={DN_PW}/UIC=[200,201]/DEVICE=SYS$SYSDEVICE:'
              '/DIRECTORY=[SYSMGR]/NOPWDEXPIRED/PWDLIFETIME=NONE/PRIVILEGES=(TMPMBX,NETMBX)'
              '/DEFPRIVILEGES=(TMPMBX,NETMBX)')
        v.cmd('MCR AUTHORIZE MODIFY DNTEST/FLAGS=NODISUSER')
        v.cmd('CREATE/DIRECTORY/OWNER=[200,201]/PROTECTION=(S:RWE,O:RWE,G:RE,W:RE) SYS$SYSDEVICE:[SYSMGR]')
        v.cmd('SET SECURITY/PROTECTION=(S:RWE,O:RWE,G:RE,W:RE) SYS$SYSDEVICE:[000000]SYSMGR.DIR')
        v.create('SYS$SYSDEVICE:[SYSMGR]BRK1.TXT', ['bracket file one', 'second record'])
        v.cmd('SET SECURITY/OWNER=[200,201]/PROTECTION=(S:RWED,O:RWED,G:RE,W) SYS$SYSDEVICE:[SYSMGR]BRK1.TXT')
        v.create('SYS$SCRATCH:MKBIG.COM', ["$ OPEN/WRITE F SYS$SYSDEVICE:[SYSMGR]BIG.TXT", "$ I = 0",
                 "$ L: I = I + 1", "$ N = F$FAO(\"!4ZL\",I)",
                 "$ WRITE F \"record ''N' of the S1 long copy, abcdefghijklmnopqrstuvwxyz0123456789\"",
                 "$ IF I .LT. 2000 THEN GOTO L", "$ CLOSE F"])
        v.cmd('@SYS$SCRATCH:MKBIG.COM', t=300)
        v.cmd('DIRECTORY/OWNER/PROTECTION/SIZE SYS$SYSDEVICE:[SYSMGR]BRK1.TXT,BIG.TXT')
        v.cmd('SET DEFAULT SYS$LOGIN')
        ev('VAXC fixtures ready')

        a = Node('A', IMG, '52:54:00:06:a5:0a', os.path.join(OUT, 'ovmxa-console.log'),
                 mcast=f'{MGRP}:{MPORT}', pcap=os.path.join(OUT, 'ovmxa.pcap'))
        a.boot_login(pw=OVMX_PW); ev('OVMXA logged in')
        def acmd(c, t=60):
            return a.cmd(c, t=t, until=OPROMPT)
        S = f'VAXC"SYSTEM {VAX_PW}"::'
        D = f'VAXC"DNTEST {DN_PW}"::'
        B = f'VAXC"DNTEST {BAD_PW}"::'
        O = f'OVMXA"SYSTEM {OVMX_PW}"::'

        def ostep(tag, c, t=240):
            ev('OVMXA', tag, 'start')
            try:
                out = acmd(c, t=t)
            except pexpect.TIMEOUT:
                ev('OVMXA', tag, f'FAIL: no prompt in {t} s; CTRL/Y')
                a.p.send('\x19'); time.sleep(3); a.p.send('\r')
                try:
                    a.p.expect(OPROMPT, timeout=120)
                except pexpect.TIMEOUT:
                    ev('OVMXA', tag, 'console did not come back after CTRL/Y')
                    raise
                out = ''
            st = acmd('SHOW SYMBOL $STATUS', t=30)
            m = re.search(r'%X[0-9A-F]+', st)
            ev('OVMXA', tag, 'done $STATUS=' + (m.group(0) if m else '?'))
            return out

        def vstep(tag, c, t=240):
            ev('VAXC', tag, 'start')
            out = v.cmd(c, t=t)
            st = v.cmd('SHOW SYMBOL $STATUS', t=30)
            m = re.search(r'%X[0-9A-F]+', st)
            ev('VAXC', tag, 'done $STATUS=' + (m.group(0) if m else '?'))
            return out

        if os.environ.get('NETACP_NL'):
            # DIAGNOSTIC VARIANT (run B), not the shipped configuration: the NETACP NCP
            # started at boot is replaced by one whose SYS$OUTPUT / SYS$ERROR are NL:,
            # so its NETACP.LOG RMS sink is off (run A: with the sink, NETACP ended
            # during the BIG COPY and at the inbound SET HOST login).
            acmd('TYPE SYS$MANAGER:NETACP.LOG', t=60)
            pid = re.search(r'([0-9A-F]{8}) NETACP', acmd('SHOW SYSTEM')).group(1)
            ostep('0a', f'STOP/IDENTIFICATION={pid}', t=60)
            time.sleep(3)
            ostep('0b', 'RUN/DETACHED/PROCESS_NAME=NETACP/INPUT=NL:/OUTPUT=NL:/ERROR=NL: '
                  'SYS$SYSTEM:NETACP.EXE', t=60)
            time.sleep(20)
            # NETACP's OPCOM "DECnet starting" lands on the console after RUN's
            # prompt; take the extra prompts before the next step
            for _ in range(3):
                try:
                    a.p.expect(OPROMPT, timeout=3)
                except pexpect.TIMEOUT:
                    break
            ostep('0c', 'SHOW SYSTEM', t=60)
        # --- (4) DECNETD.EXE removed from the running system before any DECnet command.
        ostep('4a', 'DIRECTORY/SIZE/DATE SYS$SYSTEM:DECNETD.EXE,NETACP.EXE,FAL.EXE,NCP.EXE', t=60)
        # RENAME of a SYS$SYSTEM file gives RMS-E-RNF on OVMX (rd vms-518, first attempt), so the
        # image is DELETEd from this scratch copy of the system disk.
        ostep('4b', 'DELETE SYS$SYSTEM:DECNETD.EXE;*', t=60)
        ostep('4c', 'DIRECTORY SYS$SYSTEM:DECNETD.EXE', t=60)
        ostep('4d', 'DIRECTORY SYS$SYSTEM:DECNETD.*', t=60)

        # --- (1) NETACP is its own detached process
        ostep('1a', 'SHOW SYSTEM', t=60)
        out = ostep('1b', 'TYPE SYS$MANAGER:NETACP.LOG', t=60)
        ev('1b NETACP.LOG lines with DATALINK/STARTED:',
           len(re.findall(r'DATALINK|STARTED', out)))
        acmd('NCP :== $SYS$SYSTEM:NCP.EXE', t=30)
        for _ in range(30):
            if re.search(r'State\s*=\s*on', ostep('1c', 'NCP SHOW EXECUTOR', t=60)):
                break
            time.sleep(2)
        ostep('1d', 'NCP SHOW KNOWN NODES', t=60)
        PID = re.search(r'([0-9A-F]{8})', acmd('WRITE SYS$OUTPUT F$GETJPI("","PID")', t=30)).group(1)
        ev('OVMXA console PID', PID)

        # --- (4) positive control: SET HOST is, on this branch, still the DECNETD.EXE client
        ev('OVMXA', '4e', 'start')
        a.send('SET HOST VAXC')
        k = a.p.expect([OPROMPT, 'Username:'], timeout=120)
        if k == 1:
            ev('OVMXA 4e CONTROL FAILED: SET HOST reached VAXC Username: with DECNETD.EXE renamed')
            a.send('SYSTEM'); a.p.expect('Password:', timeout=60); a.send(VAX_PW)
            a.p.expect(PROMPT, timeout=120); a.send('LOGOUT'); a.p.expect(OPROMPT, timeout=120)
        st = acmd('SHOW SYMBOL $STATUS', t=30)
        m = re.search(r'%X[0-9A-F]+', st)
        ev('OVMXA', '4e', 'done $STATUS=' + (m.group(0) if m else '?'))

        # --- (1) the VAX logs in to OVMXA (inbound CTERM, served by NETACP), SHOW SYSTEM
        def vax_sethost(tag):
            """VAXC SET HOST OVMXA; True if a remote DCL session stays up."""
            ev('VAXC SET HOST OVMXA start', tag)
            v.p.send('SET HOST OVMXA\r'); v.p.expect('Username:', timeout=120)
            v.p.send('SYSTEM\r'); v.p.expect('Password:', timeout=60); time.sleep(0.5)
            v.p.send(OVMX_PW + '\r')
            k = v.p.expect([r'REM-S-END', PROMPT, pexpect.TIMEOUT], timeout=120)
            if k == 2:
                ev('VAXC', tag, 'FAIL: no DCL prompt in 120 s after login; OVMXA console looks')
                acmd('SHOW SYSTEM'); acmd('TYPE SYS$MANAGER:NETACP.LOG')
                # VAX RTPAD: CTRL/Y twice asks "Are you repeating ^Y to abort the
                # remote session on node OVMXA?"; Y returns to VAXC
                v.p.send('\x19'); time.sleep(2); v.p.send('\x19')
                v.p.expect('abort the remote session', timeout=60)
                time.sleep(1); v.p.send('Y\r')
                v.p.expect(r'\$ $', timeout=120)
                ev('VAXC', tag, 'back on VAXC after CTRL/Y CTRL/Y')
                return False
            if k == 0:
                v.p.expect(PROMPT, timeout=60)
                ev('VAXC', tag, 'FAIL: %REM-S-END right after login; back on VAXC')
                return False
            ev('VAXC logged in to OVMXA', tag)
            vstep(tag + 'e', 'SHOW SYSTEM')
            vstep(tag + 'f', 'WRITE SYS$OUTPUT F$GETSYI("NODENAME")')
            vstep(tag + 'g', 'TYPE SYS$MANAGER:NETACP.LOG')
            v.p.send('LOGOUT\r'); v.p.expect(PROMPT, timeout=120)
            ev('VAXC back from OVMXA', tag)
            return True

        # (the inbound SET HOST of case 1 runs LAST: in the first run of this bench
        # NETACP ended at that login, and every later case then had no network)

        if os.environ.get('S3_SETHOST_LAST'):
            ev('case 1 inbound SET HOST deferred to the end (S3_SETHOST_LAST)')
        elif not vax_sethost('1'):
            ostep('1h', 'SHOW SYSTEM', t=60)
            ostep('1i', 'TYPE SYS$MANAGER:NETACP.LOG', t=60)

        # --- (3) OVMX -> VAX: the S1 RMS client path through NETACP
        ostep('3a', f'DIRECTORY {S}SYS$SYSDEVICE:[SYSMGR]*.TXT')
        ostep('3b', f'DIRECTORY {D}SYS$LOGIN:BRK1.TXT')
        ostep('3c', f'TYPE {S}SYS$SYSDEVICE:[SYSMGR]BRK1.TXT')
        ostep('3d', f'DIRECTORY {B}SYS$LOGIN:BRK1.TXT')
        # --- (2) VAX -> OVMX: DIRECTORY and COPY into OVMX's FAL (NETACP $CREPRC FAL.EXE)
        vstep('2a', f'DIRECTORY {O}')
        vstep('2b', f'DIRECTORY {O}SYS$LOGIN:*.TXT')
        ev('VAXC', '2c', 'start (OVMXA console samples SHOW SYSTEM during it)')
        v.p.send(f'COPY SYS$SYSDEVICE:[SYSMGR]BIG.TXT {O}BIGV.TXT\r')
        time.sleep(3)
        for n in range(4):
            ev('OVMXA SHOW SYSTEM sample', n)
            acmd('SHOW SYSTEM', t=60)
            time.sleep(4)
        v.p.expect(PROMPT, timeout=900)
        st = v.cmd('SHOW SYMBOL $STATUS', t=30)
        m = re.search(r'%X[0-9A-F]+', st)
        ev('VAXC', '2c', 'done $STATUS=' + (m.group(0) if m else '?'))
        ostep('2d', 'DIRECTORY/SIZE SYS$LOGIN:BIGV.TXT', t=60)
        vstep('2e', f'COPY {O}BIGV.TXT SYS$SYSDEVICE:[SYSMGR]BIGVB.TXT', t=900)
        v.cmd('DIRECTORY/SIZE=ALL SYS$SYSDEVICE:[SYSMGR]BIG.TXT,BIGVB.TXT')
        vstep('2f', 'DIFFERENCES/MAXIMUM_DIFFERENCES=3 SYS$SYSDEVICE:[SYSMGR]BIG.TXT SYS$SYSDEVICE:[SYSMGR]BIGVB.TXT', t=600)
        for f in ('BIG.TXT', 'BIGVB.TXT'):
            v.cmd(f'CHECKSUM SYS$SYSDEVICE:[SYSMGR]{f}', t=300)
            v.cmd('SHOW SYMBOL CHECKSUM$CHECKSUM')
        vstep('2g', f'DIRECTORY/SIZE {O}BIGV.TXT')
        # and the FAL-received file goes back out through OVMX's RMS client (S1 path)
        ostep('2h', f'COPY SYS$LOGIN:BIGV.TXT {S}SYS$SYSDEVICE:[SYSMGR]BIGVRT.TXT', t=420)
        vstep('2i', 'DIFFERENCES/MAXIMUM_DIFFERENCES=3 SYS$SYSDEVICE:[SYSMGR]BIG.TXT SYS$SYSDEVICE:[SYSMGR]BIGVRT.TXT', t=600)
        v.cmd('CHECKSUM SYS$SYSDEVICE:[SYSMGR]BIGVRT.TXT', t=300)
        v.cmd('SHOW SYMBOL CHECKSUM$CHECKSUM')

        # --- (3) the BIG outbound COPY last: in run 1 (fixed image) it stalled
        ostep('3e', f'COPY {S}SYS$SYSDEVICE:[SYSMGR]BIG.TXT SYS$LOGIN:BIG.TXT', t=420)
        ostep('3f', 'DIRECTORY/SIZE SYS$LOGIN:BIG.TXT', t=60)
        ostep('3g', f'COPY SYS$LOGIN:BIG.TXT {S}SYS$SYSDEVICE:[SYSMGR]BIGRT.TXT', t=420)
        v.cmd('DIRECTORY/SIZE=ALL SYS$SYSDEVICE:[SYSMGR]BIG.TXT,BIGRT.TXT')
        vstep('3h', 'DIFFERENCES/MAXIMUM_DIFFERENCES=3 SYS$SYSDEVICE:[SYSMGR]BIG.TXT SYS$SYSDEVICE:[SYSMGR]BIGRT.TXT', t=420)
        for f in ('BIG.TXT', 'BIGRT.TXT'):
            v.cmd(f'CHECKSUM SYS$SYSDEVICE:[SYSMGR]{f}', t=300)
            v.cmd('SHOW SYMBOL CHECKSUM$CHECKSUM')

        # --- (4)/(1) after everything: the process table, the log, DECNETD.EXE still absent
        ostep('4f', 'SHOW SYSTEM', t=60)
        ostep('4g', 'TYPE SYS$MANAGER:NETACP.LOG', t=60)
        ostep('4h', 'DIRECTORY SYS$SYSTEM:DECNETD.*', t=60)
        if not vax_sethost('9'):      # a second inbound login, after every file case
            ostep('9h', 'SHOW SYSTEM', t=60)
            ostep('9i', 'TYPE SYS$MANAGER:NETACP.LOG', t=60)
        v.cmd('SHOW ERROR')
        v.cmd('MCR NCP SHOW NODE OVMXA COUNTERS')
        ev('DONE')
    finally:
        if a:
            a.close()
        subprocess.run(['docker', 'rm', '-f', CTR], capture_output=True)
        relay.terminate()

if __name__ == '__main__':
    main()
