#!/usr/bin/env python3
"""rd vms-3106 live re-check bench.

VAXC  = real OpenVMS VAX V7.3 (Node C DECnet disk, scratch copy) under SIMH in a
        local container, licensed at run time from the lab PAKs (never logged).
OVMXA = x86 OVMX V0.7-14 release image under KVM, DECnet 1.963 injected by
        tools/cluster-web-demo/inject-decnet-config.sh, and a DECNETD.EXE;2 put on
        SYS$SYSTEM for each variant (main = origin/main, fix = the PR branch).
Both on one L2 segment: SIMH udp: Ethernet <-> udp2mcast.py <-> qemu socket mcast.

For each variant: boot OVMXA, SET HOST VAXC and log in, then KILL qemu mid-session.
Reboot OVMXA at once and SET HOST VAXC again while VAXC still holds the dead link.

usage: lla_reboot.py <outdir> <variant>[,<variant>...]
"""
import json, os, re, subprocess, sys, time
import pexpect
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from node import Node

W = os.environ.get('W', '/tmp/vms3106-lab')
OUT = sys.argv[1]
VARIANTS = sys.argv[2].split(',')
VAX_PW = os.environ['VAX_PW']    # the lab test account (vms-aa2: never a literal)
MCAST = '230.0.0.31:12431'
os.makedirs(OUT, exist_ok=True)
EVENTS = open(os.path.join(OUT, 'events.log'), 'a')

def ev(*a):
    s = time.strftime('%H:%M:%S ') + ' '.join(str(x) for x in a)
    print(s, flush=True); EVENTS.write(s + '\n'); EVENTS.flush()

# ---------------------------------------------------------------- VAXC (SIMH)
class Vax:
    def __init__(self):
        subprocess.run(['docker', 'rm', '-f', 'vms3106-vaxc'], capture_output=True)
        self.log = open(os.path.join(OUT, 'vaxc-console.log'), 'ab')
        self.p = pexpect.spawn('docker', ['run', '--rm', '-it', '--name', 'vms3106-vaxc',
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

    def cmd(self, c, t=120):
        self.p.send(c + '\r'); self.p.expect(r'\r\n[\r\x00]*\$ $', timeout=t)
        return self.p.before.decode('latin1', 'replace')

    def license(self):
        """Register DVNETRTG, VAX-VMS, VAXCLUSTER from the lab PAKs. The console
        log is PAUSED while the PAK lines are typed: they never reach a file."""
        paks = json.load(open(f'{W}/secret/paks3.json'))
        try:
            self._register(paks)
        except Exception:
            # never let a pexpect traceback print the console buffer (it holds PAK text)
            raise RuntimeError('VAXC: LICENSE REGISTER did not complete (buffer suppressed)') from None
        self.p.logfile_read = self.log
        self.p.send('\r'); self.p.expect(r'\$ $', timeout=30)
        for prod in ('DVNETRTG', 'VAX-VMS', 'VAXCLUSTER'):
            self.cmd('LICENSE LOAD ' + prod)
        self.cmd('SHOW LICENSE')

    def _register(self, paks):
        self.p.logfile_read = None
        self.log.write(b'\r\n[bench: LICENSE REGISTER of DVNETRTG, VAX-VMS, VAXCLUSTER typed here from the '
                       b'lab PAKs. The console log is paused; PAK contents are REDACTED ENTIRELY.]\r\n')
        for blk in paks:
            for k, line in enumerate(blk):
                line = line[2:] if k == 0 and line.startswith('$ ') else line
                self.p.send(line + '\r')
                self.p.expect([r'_\$ $', r'\r\n[\r\x00]*\$ $'], timeout=60)
                time.sleep(0.3)

    def links(self, tag):
        txt = self.cmd('MCR NCP SHOW KNOWN LINKS')
        ev('VAXC links', tag, '|', ' / '.join(l.strip() for l in txt.splitlines() if l.strip() and 'NCP' not in l))
        return txt

# ---------------------------------------------------------------- OVMXA (KVM)
def boot_ovmx(variant, n):
    img = f'{W}/ovmxa-{variant}.img'
    a = Node('A', img, '52:54:00:31:06:0a', os.path.join(OUT, f'{variant}-boot{n}-ovmxa-console.log'),
             mcast=MCAST, pcap=os.path.join(OUT, f'{variant}-boot{n}.pcap'))
    t0 = time.time(); a.boot_login(); ev(variant, f'boot{n}: OVMXA logged in after {time.time()-t0:.0f}s')
    a.cmd('DIRECTORY/SIZE/DATE SYS$SYSTEM:DECNETD.EXE;*', t=60)
    a.cmd('NCP :== $SYS$SYSTEM:NCP.EXE', t=30)
    # NETACP prints "DECnet starting"; give it until the executor is on.
    for _ in range(30):
        out = a.cmd('NCP SHOW EXECUTOR', t=60)
        if re.search(r'State\s*=\s*on', out):
            break
        time.sleep(2)
    ev(variant, f'boot{n}: NCP executor on={bool(re.search(r"State\s*=\s*on", out))}')
    return a

def sethost(a, variant, n, t=180):
    t0 = time.time()
    a.send('SET HOST VAXC')
    i = a.p.expect([r'Username: ', r'%[A-Z]+-[EFW]-[A-Z]+,[^\r\n]*', pexpect.TIMEOUT], timeout=t)
    dt = time.time() - t0
    if i != 0:
        r = a.p.after.decode('latin1') if i == 1 else f'TIMEOUT after {t}s'
        ev(variant, f'boot{n}: SET HOST VAXC -> FAIL ({r}) after {dt:.1f}s')
        return False, dt
    ev(variant, f'boot{n}: SET HOST VAXC -> Username: after {dt:.1f}s')
    time.sleep(1); a.send('SYSTEM')
    a.p.expect('Password:', timeout=60); time.sleep(1); a.send(VAX_PW)
    a.p.expect(r'\$ ', timeout=180); time.sleep(2)
    a.send('WRITE SYS$OUTPUT "WHO=",F$GETSYI("NODENAME")," TT=",F$GETDVI("TT","FULLDEVNAM")')
    a.p.expect(r'WHO=[A-Z0-9]+ TT=[^\r\n]*', timeout=120); ev(variant, f'boot{n}: remote', a.p.after.decode('latin1'))
    a.p.expect(r'\$ ', timeout=60)
    return True, dt

def main():
    relay = subprocess.Popen([sys.executable, os.path.join(HERE, 'udp2mcast.py'), '230.0.0.31', '12431',
                              '41001', '41002'], stdout=open(os.path.join(OUT, 'relay.log'), 'a'),
                             stderr=subprocess.STDOUT)
    v = Vax()
    res = {}
    try:
        v.boot(); ev('VAXC at Username:'); v.login(); ev('VAXC logged in')
        v.license(); ev('VAXC licensed')
        # STARTNET at boot fails NOLICENSE on a fresh copy; with the PAKs now in
        # the LMF database it is re-run. (A copy that already holds them started
        # DECnet at boot, and the re-run only prints DUPLNAM noise.)
        v.cmd('@SYS$STARTUP:STARTNET', t=300)
        for _ in range(60):
            if re.search(r'State\s*=\s*on', v.cmd('MCR NCP SHOW EXECUTOR')):
                break
            time.sleep(5)
        else:
            raise RuntimeError('VAXC: DECnet executor never reached state on')
        v.cmd('MCR NCP SHOW KNOWN CIRCUITS')
        ev('VAXC DECnet executor on')
        for var in VARIANTS:
            r = res[var] = {}
            a = boot_ovmx(var, 1)
            ok, dt = sethost(a, var, 1); r['boot1'] = (ok, round(dt, 1))
            v.links(var + ' boot1 session up')
            time.sleep(3)
            a.close(); ev(var, 'boot1: qemu KILLED mid-session (SIGKILL)'); t_kill = time.time()
            a = boot_ovmx(var, 2)
            v.links(var + f' boot2 before SET HOST ({time.time()-t_kill:.0f}s after kill)')
            ok, dt = sethost(a, var, 2); r['boot2'] = (ok, round(dt, 1))
            v.links(var + ' boot2 after SET HOST')
            if ok:
                a.send('LOGOUT')
                a.p.expect(r'%REM-S-END[^\r\n]*', timeout=120); ev(var, 'boot2:', a.p.after.decode('latin1'))
                a.p.expect(r'\$ ', timeout=60)
            time.sleep(2); a.close(); ev(var, 'boot2: qemu stopped')
            # let VAXC forget every link from 1.963 before the next variant
            for _ in range(60 if var != VARIANTS[-1] else 0):
                if '1.963' not in v.links(var + ' drain'):
                    break
                time.sleep(15)
        ev('RESULTS', json.dumps(res))
        v.cmd('MCR NCP SHOW EXECUTOR COUNTERS')
    finally:
        subprocess.run(['docker', 'rm', '-f', 'vms3106-vaxc'], capture_output=True)
        relay.terminate()

if __name__ == '__main__':
    main()
