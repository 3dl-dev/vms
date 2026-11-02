#!/usr/bin/env python3
"""rd vms-0a7 S1 live re-proof (copied from the vms-6a4 bench): OVMX DCL DIRECTORY /
TYPE / COPY node::spec, now plain RMS clients ($PARSE/$SEARCH/$OPEN/$GET/$PUT over
_NET: + IO$_ACCESS + DAP), against a real OpenVMS VAX V7.3 over DECnet (FAL, object
17), and the VAX's COPY / DIRECTORY against OVMX's FAL. Added over vms-6a4: a wrong
password, COPY/LOG of a remote wildcard, a 2000-record COPY each way, a VAX-loopback
oracle for the access-string and wrong-password cases, a process-logical control that
makes SYS$SYSTEM:DECNETD.EXE unreachable from the console process (positive control:
SET HOST, which still activates it), and a second login (VAXC SET HOST OVMXA) that
samples SHOW SYSTEM / JPI$_IMAGNAME during a 20000-record COPY, and BIG.TXT pushed
by VAXC into OVMX's FAL then sent back by OVMXA's RMS client.

VAXC  = real OpenVMS VAX V7.3 under SIMH (Node C DECnet disk, scratch copy),
        licensed at run time from the lab PAKs (console log paused; never logged).
OVMXA = x86 OVMX under KVM, image built from the branch under test, DECnet 1.963
        injected by tools/cluster-web-demo/inject-decnet-config.sh.
Bridged by udp2mcast.py, as in ../vms-3106-lla-reboot-20261102/.

usage: fal_verbs.py <outdir> <ovmx-disk-image>       (env REL = boot artifacts dir)
"""
import json, os, re, subprocess, sys, time
import pexpect
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from node import Node

W = os.environ.get('W', '/tmp/vms0a7-lab')
OUT, IMG = sys.argv[1], sys.argv[2]
VAX_PW = 'OVMXCLUSTER1'          # lab test SYSTEM account on VAXC
DN_PW = 'DNTESTPW1'              # a throwaway unprivileged account this bench creates
OVMX_PW = 'MANAGER'
MCAST = '230.0.0.71:12471'
BAD_PW = 'NOTTHEPW9'           # deliberately wrong; must never be echoed back
os.makedirs(OUT, exist_ok=True)
EVENTS = open(os.path.join(OUT, 'events.log'), 'a')

def ev(*a):
    s = time.strftime('%H:%M:%S ') + ' '.join(str(x) for x in a)
    print(s, flush=True); EVENTS.write(s + '\n'); EVENTS.flush()

PROMPT = r'\r\n[\r\x00]*\$ $'

class Vax:
    def __init__(self):
        subprocess.run(['docker', 'rm', '-f', 'vms0a7-vaxc'], capture_output=True)
        self.log = open(os.path.join(OUT, 'vaxc-console.log'), 'ab')
        self.p = pexpect.spawn('docker', ['run', '--rm', '-it', '--name', 'vms0a7-vaxc',
                               '--network', 'host', '--user', f'{os.getuid()}:{os.getgid()}',
                               '-v', f'{W}/vaxw:/w', '-w', '/w', '--entrypoint', '/usr/local/bin/vax',
                               '192.168.2.43:30500/ovmx-vaxlab:5', 'vax.ini'], timeout=60)
        self.p.logfile_read = self.log

    def slow(self, s):
        for ch in s:
            self.p.send(ch); time.sleep(0.15)
        self.p.send('\r')

    def boot(self):
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
        self.p.send('SYSTEM\r'); self.p.expect('Password:', timeout=60); time.sleep(1)
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
    relay = subprocess.Popen([sys.executable, os.path.join(HERE, 'udp2mcast.py'), '230.0.0.71', '12471',
                              '41071', '41072'], stdout=open(os.path.join(OUT, 'relay.log'), 'a'),
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
        # --- VAXC fixtures: the oracle's layout (../decnet-fal-verbs-20261008/vax-to-vax-sys-login/)
        v.cmd('SET DEFAULT SYS$SYSTEM')
        v.cmd(f'MCR AUTHORIZE ADD DNTEST/PASSWORD={DN_PW}/UIC=[200,201]/DEVICE=SYS$SYSDEVICE:'
              '/DIRECTORY=[SYSMGR]/NOPWDEXPIRED/PWDLIFETIME=NONE/PRIVILEGES=(TMPMBX,NETMBX)'
              '/DEFPRIVILEGES=(TMPMBX,NETMBX)')
        v.cmd('MCR AUTHORIZE MODIFY DNTEST/FLAGS=NODISUSER')   # a new UAF record starts DISUSER
        v.cmd('DELETE SYS$SYSDEVICE:[SYSMGR]BRK*.TXT;*,BIG*.TXT;*,LONG*.TXT;*,SYS$LOGIN:V2O*.TXT;*,SYS$SYSDEVICE:[LOOPOUT]*.*;*')  # a re-run starts clean
        v.cmd('CREATE/DIRECTORY SYS$SYSDEVICE:[LOOPOUT]')
        v.cmd('CREATE/DIRECTORY/OWNER=[200,201]/PROTECTION=(S:RWE,O:RWE,G:RE,W:RE) SYS$SYSDEVICE:[SYSMGR]')
        v.cmd('SET SECURITY/PROTECTION=(S:RWE,O:RWE,G:RE,W:RE) SYS$SYSDEVICE:[000000]SYSMGR.DIR')
        v.create('SYS$SYSDEVICE:[SYSMGR]BRK1.TXT', ['bracket file one', 'second record'])
        v.create('SYS$SYSDEVICE:[SYSMGR]BRK2.TXT', ['bracket file two'])
        v.create('SYS$SYSDEVICE:[SYSMGR]BRKP.TXT', ['protected file'])
        for f in ('BRK1.TXT', 'BRK2.TXT'):
            v.cmd(f'SET SECURITY/OWNER=[200,201]/PROTECTION=(S:RWED,O:RWED,G:RE,W) SYS$SYSDEVICE:[SYSMGR]{f}')
        v.cmd('SET SECURITY/OWNER=[1,4]/PROTECTION=(S:RWED,O:RWED,G,W) SYS$SYSDEVICE:[SYSMGR]BRKP.TXT')
        # BIG.TXT: 2000 VAR/CR records, written by DCL itself, for the long COPY each way
        v.create('SYS$SCRATCH:MKBIG.COM', ["$ OPEN/WRITE F SYS$SYSDEVICE:[SYSMGR]BIG.TXT", "$ I = 0",
                 "$ L: I = I + 1", "$ N = F$FAO(\"!4ZL\",I)",
                 "$ WRITE F \"record ''N' of the S1 long copy, abcdefghijklmnopqrstuvwxyz0123456789\"",
                 "$ IF I .LT. 2000 THEN GOTO L", "$ CLOSE F"])
        v.cmd('@SYS$SCRATCH:MKBIG.COM', t=300)
        # LONG.TXT: 20000 records, so a COPY lasts long enough to sample OVMXA from a second login
        v.create('SYS$SCRATCH:MKLONG.COM', ["$ OPEN/WRITE F SYS$SYSDEVICE:[SYSMGR]LONG.TXT", "$ I = 0",
                 "$ L: I = I + 1", "$ N = F$FAO(\"!5ZL\",I)",
                 "$ WRITE F \"long record ''N' for the S1 second-login sample, abcdefghijklmnopqrstuvwxyz\"",
                 "$ IF I .LT. 20000 THEN GOTO L", "$ CLOSE F"])
        v.cmd('@SYS$SCRATCH:MKLONG.COM', t=900)
        v.cmd('DIRECTORY/OWNER/PROTECTION/SIZE SYS$SYSDEVICE:[SYSMGR]BRK*.TXT,BIG.TXT,LONG.TXT')
        v.cmd('DIRECTORY SYS$LOGIN:*.COM')
        v.cmd('SET DEFAULT SYS$LOGIN')
        ev('VAXC fixtures ready')

        a = Node('A', IMG, '52:54:00:06:a4:0a', os.path.join(OUT, 'ovmxa-console.log'),
                 mcast=MCAST, pcap=os.path.join(OUT, 'ovmxa.pcap'))
        a.boot_login(); ev('OVMXA logged in')
        a.cmd('NCP :== $SYS$SYSTEM:NCP.EXE', t=30)
        for _ in range(30):
            if re.search(r'State\s*=\s*on', a.cmd('NCP SHOW EXECUTOR', t=60)):
                break
            time.sleep(2)
        a.cmd('NCP SHOW KNOWN NODES', t=60)
        S = f'VAXC"SYSTEM {VAX_PW}"::'
        D = f'VAXC"DNTEST {DN_PW}"::'
        B = f'VAXC"DNTEST {BAD_PW}"::'
        def ostep(tag, c, t=240):
            ev('OVMXA', tag, 'start')
            a.cmd(c, t=t)
            st = a.cmd('SHOW SYMBOL $STATUS', t=30)
            m = re.search(r'%X[0-9A-F]+', st)
            ev('OVMXA', tag, 'done $STATUS=' + (m.group(0) if m else '?'))
        def vstep(tag, c, t=240):
            ev('VAXC', tag, 'start')
            v.cmd(c, t=t)
            st = v.cmd('SHOW SYMBOL $STATUS', t=30)
            m = re.search(r'%X[0-9A-F]+', st)
            ev('VAXC', tag, 'done $STATUS=' + (m.group(0) if m else '?'))
        PID = re.search(r'([0-9A-F]{8})', a.cmd('WRITE SYS$OUTPUT F$GETJPI("","PID")', t=30)).group(1)
        ev('OVMXA console PID', PID)
        a.cmd('SHOW SYSTEM', t=60)
        a.cmd('DIRECTORY/SIZE SYS$SYSTEM:DECNETD.EXE', t=60)
        # --- (0) DECNETD.EXE unreachable from THIS process only: a process logical makes
        # SYS$SYSTEM: name SYS$LOGIN:, which holds no DECNETD.EXE. NETACP (another process,
        # already running from DECNETD.EXE) is untouched. Positive control: SET HOST, which
        # on this branch still activates SYS$SYSTEM:DECNETD.EXE, must now find no image.
        ostep('0a', 'DEFINE/PROCESS SYS$SYSTEM SYS$LOGIN:', t=30)
        a.cmd('SHOW LOGICAL SYS$SYSTEM', t=30)
        a.cmd('DIRECTORY SYS$SYSTEM:DECNETD.EXE', t=30)
        ev('OVMXA', '0b', 'start')
        a.send('SET HOST VAXC')
        k = a.p.expect([r'\$ ', 'Username:'], timeout=120)
        if k == 1:      # control did NOT block activation: log in and leave, record it
            ev('OVMXA 0b CONTROL FAILED: SET HOST reached VAXC Username:')
            a.send('SYSTEM'); a.p.expect('Password:', timeout=60); a.send(VAX_PW)
            a.p.expect(r'\$ ', timeout=120); a.send('LOGOUT'); a.p.expect(r'\$ ', timeout=120)
        st = a.cmd('SHOW SYMBOL $STATUS', t=30)
        m = re.search(r'%X[0-9A-F]+', st)
        ev('OVMXA', '0b', 'done $STATUS=' + (m.group(0) if m else '?'))
        for tag, c in [('0c', f'DIRECTORY {S}SYS$SYSDEVICE:[SYSMGR]BRK*.TXT'),
                       ('0d', f'TYPE {S}SYS$SYSDEVICE:[SYSMGR]BRK1.TXT'),
                       ('0e', f'COPY {S}SYS$SYSDEVICE:[SYSMGR]BRK2.TXT SYS$LOGIN:BRK2C.TXT'),
                       ('0f', f'COPY SYS$LOGIN:BRK2C.TXT {S}SYS$SYSDEVICE:[SYSMGR]BRK2RT.TXT'),
                       ('0g', f'COPY {S}SYS$SYSDEVICE:[SYSMGR]BIG.TXT SYS$LOGIN:BIGC.TXT')]:
            ostep(tag, c)
        ostep('0h', 'DEASSIGN/PROCESS SYS$SYSTEM', t=30)
        a.cmd('SHOW LOGICAL SYS$SYSTEM', t=30)
        steps = [
            ('1a', f'DIRECTORY {S}SYS$LOGIN:*.COM'),
            ('1b', f'DIRECTORY {S}SYS$LOGIN:NOSUCH*.XYZ'),
            ('1c', f'DIRECTORY {S}SYS$SYSDEVICE:[SYSMGR]BRK*.TXT'),
            ('1d', f'DIRECTORY {D}SYS$LOGIN:BRK*.TXT'),
            ('1e', f'DIRECTORY {D}SYS$LOGIN:NOSUCH.TXT'),
            ('1f', f'DIRECTORY/FULL {D}SYS$LOGIN:BRK*.TXT'),
            ('2a', f'TYPE {S}SYS$SYSDEVICE:[SYSMGR]BRK1.TXT'),
            ('2b', f'TYPE {S}SYS$SYSDEVICE:[SYSMGR]BRK*.TXT'),
            ('2c', f'TYPE {D}SYS$LOGIN:BRK*.TXT'),
            ('2d', f'TYPE {D}SYS$LOGIN:BRKP.TXT'),
            ('5a', f'DIRECTORY {B}SYS$LOGIN:BRK*.TXT'),
            ('5b', f'TYPE {B}SYS$LOGIN:BRK1.TXT'),
            ('3a', f'COPY {S}SYS$SYSDEVICE:[SYSMGR]BRK1.TXT SYS$LOGIN:BRK1.TXT'),
            ('3b', 'TYPE SYS$LOGIN:BRK1.TXT'),
            ('3c', f'COPY SYS$LOGIN:BRK1.TXT {S}SYS$SYSDEVICE:[SYSMGR]BRK1RT.TXT'),
            ('3d', f'COPY/LOG {S}SYS$SYSDEVICE:[SYSMGR]BRK*.TXT SYS$LOGIN:*.*'),
            ('3e', 'DIRECTORY SYS$LOGIN:BRK*.TXT'),
            ('3f', f'COPY {S}SYS$SYSDEVICE:[SYSMGR]BIG.TXT SYS$LOGIN:BIG.TXT'),
            ('3g', 'DIRECTORY/SIZE SYS$LOGIN:BIG.TXT'),
            ('3h', f'COPY SYS$LOGIN:BIG.TXT {S}SYS$SYSDEVICE:[SYSMGR]BIGRT.TXT'),
        ]
        for tag, c in steps:
            ostep(tag, c)
        # --- (6) a second login on OVMXA (VAXC SET HOST OVMXA, CTERM) samples SHOW SYSTEM and
        # the console process's JPI$_IMAGNAME while the console runs a 20000-record COPY
        ev('VAXC SET HOST OVMXA start')
        v.p.send('SET HOST OVMXA\r'); v.p.expect('Username:', timeout=120)
        v.p.send('SYSTEM\r'); v.p.expect('Password:', timeout=60); time.sleep(0.5)
        v.p.send(OVMX_PW + '\r'); v.p.expect(PROMPT, timeout=120)
        ev('VAXC logged in to OVMXA')
        v.cmd('SHOW SYSTEM')
        ev('OVMXA', '6a', 'start')
        a.send(f'COPY {S}SYS$SYSDEVICE:[SYSMGR]LONG.TXT SYS$LOGIN:LONG.TXT')
        time.sleep(2)
        for n in range(3):
            ev('second login sample', n)
            v.cmd('SHOW SYSTEM')
            v.cmd(f'WRITE SYS$OUTPUT "IMAGNAME=<" + F$GETJPI("{PID}","IMAGNAME") + ">"')
            v.cmd(f'SHOW PROCESS/ID={PID}')
            time.sleep(1)
        a.p.expect(r'\$ ', timeout=900)
        st = a.cmd('SHOW SYMBOL $STATUS', t=30)
        m = re.search(r'%X[0-9A-F]+', st)
        ev('OVMXA', '6a', 'done $STATUS=' + (m.group(0) if m else '?'))
        v.cmd('SHOW SYSTEM')
        v.p.send('LOGOUT\r'); v.p.expect(PROMPT, timeout=120)
        ev('VAXC back from OVMXA')
        ostep('6b', f'COPY SYS$LOGIN:LONG.TXT {S}SYS$SYSDEVICE:[SYSMGR]LONGRT.TXT', t=900)
        a.cmd('SHOW SYSTEM', t=60)
        # --- (3) byte identity, judged on the VAX
        for x, y in (('BRK1.TXT', 'BRK1RT.TXT'), ('BRK2.TXT', 'BRK2RT.TXT'), ('BIG.TXT', 'BIGRT.TXT'), ('LONG.TXT', 'LONGRT.TXT')):
            v.cmd(f'DIRECTORY/FULL SYS$SYSDEVICE:[SYSMGR]{x},{y}')
            vstep('3v-' + x, f'DIFFERENCES/MAXIMUM_DIFFERENCES=3 SYS$SYSDEVICE:[SYSMGR]{x} SYS$SYSDEVICE:[SYSMGR]{y}')
            for f in (x, y):
                v.cmd(f'CHECKSUM SYS$SYSDEVICE:[SYSMGR]{f}')
                v.cmd('SHOW SYMBOL CHECKSUM$CHECKSUM')
        v.cmd('DUMP/RECORDS SYS$SYSDEVICE:[SYSMGR]BRK1.TXT')
        v.cmd('DUMP/RECORDS SYS$SYSDEVICE:[SYSMGR]BRK1RT.TXT')
        ev('VAXC (3) compare done')
        # --- VAX-loopback oracle (same run, same fixtures): VAXC's own DCL against VAXC's FAL
        for tag, c in [('L1d', f'DIRECTORY {D}SYS$LOGIN:BRK*.TXT'),
                       ('L1e', f'DIRECTORY {D}SYS$LOGIN:NOSUCH.TXT'),
                       ('L1f', f'DIRECTORY/FULL {D}SYS$LOGIN:BRK*.TXT'),
                       ('L2c', f'TYPE {D}SYS$LOGIN:BRK*.TXT'),
                       ('L2d', f'TYPE {D}SYS$LOGIN:BRKP.TXT'),
                       ('L5a', f'DIRECTORY {B}SYS$LOGIN:BRK*.TXT'),
                       ('L5b', f'TYPE {B}SYS$LOGIN:BRK1.TXT'),
                       ('L3d', f'COPY/LOG {S}SYS$SYSDEVICE:[SYSMGR]BRK*.TXT SYS$SYSDEVICE:[LOOPOUT]*.*')]:
            vstep(tag, c)
        # --- (4) from the VAX, against OVMX's FAL
        O = f'OVMXA"SYSTEM {OVMX_PW}"::'
        v.create('SYS$LOGIN:V2O.TXT', ['vax to ovmx line one', 'line two of the round trip'])
        for tag, c in [('4a', f'COPY SYS$LOGIN:V2O.TXT {O}V2O.TXT'),
                       ('4b', f'COPY {O}V2O.TXT SYS$LOGIN:V2O_BACK.TXT'),
                       ('4c', 'DIFFERENCES SYS$LOGIN:V2O.TXT SYS$LOGIN:V2O_BACK.TXT'),
                       ('4d', 'DUMP/RECORDS SYS$LOGIN:V2O.TXT'),
                       ('4e', 'DUMP/RECORDS SYS$LOGIN:V2O_BACK.TXT'),
                       ('4f', f'DIRECTORY {O}'),
                       ('4g', f'DIRECTORY {O}V2O.TXT')]:
            vstep(tag, c)
        for f in ('V2O.TXT', 'V2O_BACK.TXT'):
            v.cmd(f'CHECKSUM SYS$LOGIN:{f}')
            v.cmd('SHOW SYMBOL CHECKSUM$CHECKSUM')
        v.cmd('DIRECTORY/FULL SYS$LOGIN:V2O.TXT,V2O_BACK.TXT')
        a.cmd('DIRECTORY SYS$LOGIN:V2O.TXT', t=60)
        a.cmd('TYPE SYS$LOGIN:V2O.TXT', t=60)
        # --- (7) the large file the other way: VAXC pushes BIG.TXT into OVMX's FAL, then
        # OVMXA's RMS client sends it back to VAXC; identity judged on VAXC
        vstep('7a', f'COPY SYS$SYSDEVICE:[SYSMGR]BIG.TXT {O}BIGV.TXT', t=900)
        ostep('7b', 'DIRECTORY/SIZE SYS$LOGIN:BIGV.TXT', t=60)
        ostep('7c', f'COPY SYS$LOGIN:BIGV.TXT {S}SYS$SYSDEVICE:[SYSMGR]BIGVRT.TXT', t=900)
        v.cmd('DIRECTORY/FULL SYS$SYSDEVICE:[SYSMGR]BIG.TXT,BIGVRT.TXT')
        vstep('7d', 'DIFFERENCES/MAXIMUM_DIFFERENCES=3 SYS$SYSDEVICE:[SYSMGR]BIG.TXT SYS$SYSDEVICE:[SYSMGR]BIGVRT.TXT', t=600)
        for f in ('BIG.TXT', 'BIGVRT.TXT'):
            v.cmd(f'CHECKSUM SYS$SYSDEVICE:[SYSMGR]{f}', t=300)
            v.cmd('SHOW SYMBOL CHECKSUM$CHECKSUM')
        ev('DONE')
    finally:
        if a:
            a.close()
        subprocess.run(['docker', 'rm', '-f', 'vms0a7-vaxc'], capture_output=True)
        relay.terminate()

if __name__ == '__main__':
    main()
