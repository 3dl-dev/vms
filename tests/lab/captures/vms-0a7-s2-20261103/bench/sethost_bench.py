#!/usr/bin/env python3
"""rd vms-0a7 S2 live bench: OVMX DCL SET HOST (SYS$SYSTEM:RTPAD.EXE, CTERM over
$QIO on _NET:) against a real OpenVMS VAX V7.3, with the VAX's own SET HOST
(VAX->VAX loopback, and VAX->OVMX) run in the same session as the oracle.

VAXC  = real OpenVMS VAX V7.3 (Node C DECnet disk, scratch copy) under SIMH in
        a local container, licensed at run time from the lab PAKs (console log
        paused while they are typed; never logged).
OVMXA = x86 OVMX under KVM, the whole image built from the S2 branch
        (distro/Dockerfile.bootable --target runner), DECnet 1.963 injected by
        tools/cluster-web-demo/inject-decnet-config.sh --scsnode OVMXA.
One L2 segment: SIMH udp: Ethernet <-> udp2mcast.py <-> qemu socket mcast.
Own container (vms0a7s2-vaxc), multicast group 230.0.0.83:12483, SIMH UDP
41081/41082, work dir /tmp/vms0a7s2-lab (S1 used vms0a7-vaxc/230.0.0.71).

Passwords come from the environment (VAX_PW, OVMX_PW, BAD_PW) and never
appear in this file; redact.py replaces them with XX in every output file.

usage: sethost_bench.py <outdir>
"""
import json, os, re, subprocess, sys, time
import pexpect
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from node import Node

W = os.environ.get('W', '/tmp/vms0a7s2-lab')
OUT = sys.argv[1]
VAX_PW, OVMX_PW, BAD_PW = os.environ['VAX_PW'], os.environ['OVMX_PW'], os.environ['BAD_PW']
VAX_PW2 = os.environ['VAX_PW2']
MCAST_G, MCAST_P = '230.0.0.83', 12483
CTR = 'vms0a7s2-vaxc'
os.makedirs(OUT, exist_ok=True)
EVENTS = open(os.path.join(OUT, 'events.log'), 'a')
CASES = open(os.path.join(OUT, 'cases.log'), 'a')
PROMPT = r'\n[\r\x00]*\$ $'
USERP = r'[\r\n]Username: (?! )'   # not OPCOM's 'Username:          X' field


def ev(*a):
    s = time.strftime('%H:%M:%S ') + ' '.join(str(x) for x in a)
    print(s, flush=True); EVENTS.write(s + '\n'); EVENTS.flush()


def flat(b):
    return b.decode('latin1', 'replace') if isinstance(b, bytes) else b


def case(tag, text):
    """One case transcript (what the screen showed for one step)."""
    t = flat(text).replace('\x00', '')
    CASES.write(f'==== {tag}\n{t}\n'); CASES.flush()
    ev(tag, '->', t.strip().replace('\r\n', ' | ').replace('\n', ' | ')[:400])


class Vax:
    def __init__(self):
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
        booted = False
        end = time.time() + 900
        while time.time() < end:
            i = self.p.expect([r'>>>', r'ENTER DATE AND TIME', r'Welcome to OpenVMS[^\n]*\n[\r\n]*Username: ?$', pexpect.TIMEOUT], timeout=20)
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
        # the console may still be printing startup/OPCOM text: retry until
        # LOGINOUT has read the username
        for _ in range(8):
            time.sleep(2); self.p.send('SYSTEM\r')
            if self.p.expect(['Password:', pexpect.TIMEOUT], timeout=30) == 0:
                break
        else:
            raise RuntimeError('VAXC: no Password: prompt')
        time.sleep(1)
        self.p.send(VAX_PW + '\r')
        if self.p.expect([r'\$ $', r'New password: ?'], timeout=180) == 1:
            # the scratch disk's SYSTEM password has expired at today's date:
            # take a one-off new one, then give SYSTEM its bench password back
            # with no expiry (AUTHORIZE, so SET PASSWORD's history does not apply)
            time.sleep(1); self.p.send(VAX_PW2 + '\r')
            self.p.expect(r'Verification: ?', timeout=60); time.sleep(1); self.p.send(VAX_PW2 + '\r')
            self.p.expect(r'\$ $', timeout=180)
            ev('VAXC: SYSTEM password had expired; reset')
            self.cmd('SET DEFAULT SYS$SYSTEM')
            self.cmd(f'MCR AUTHORIZE MODIFY SYSTEM/PASSWORD={VAX_PW}/NOPWDEXPIRED/PWDLIFETIME=NONE')
            self.cmd('SET DEFAULT SYS$LOGIN')

    def cmd(self, c, t=120):
        self.p.send(c + '\r'); self.p.expect(PROMPT, timeout=t)
        return flat(self.p.before)

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


def boot_ovmx():
    img = f'{W}/ovmxa.img'
    a = Node('A', img, '52:54:00:0a:72:0a', os.path.join(OUT, 'ovmxa-console.log'),
             mcast=f'{MCAST_G}:{MCAST_P}', pcap=os.path.join(W, 'ovmxa.pcap'),
             kernel=f'{W}/rel/vmlinuz', initrd=f'{W}/rel/initramfs-ovmx-slim.cpio.gz')
    t0 = time.time(); a.boot_login(pw=OVMX_PW); ev(f'OVMXA logged in after {time.time()-t0:.0f}s')
    a.cmd('NCP :== $SYS$SYSTEM:NCP.EXE', t=30)
    for _ in range(30):
        out = a.cmd('NCP SHOW EXECUTOR', t=60)
        if re.search(r'State\s*=\s*on', out):
            break
        time.sleep(2)
    ev('OVMXA NCP executor on =', bool(re.search(r'State\s*=\s*on', out)))
    return a


# ---------------------------------------------------------------- one session

def sh(p, c, t=120):
    p.send(c + '\r'); p.expect(PROMPT, timeout=t)
    return flat(p.before)


def remote_session(p, who, target, user, pw, local_node, inbound_check=None):
    """SET HOST <target> on the terminal driven by pexpect handle p, then the
    case list. Every case transcript is tagged '<who>/<case>'."""
    t0 = time.time()
    p.send(f'SET HOST {target}\r')
    i = p.expect([USERP, r'%[A-Z]+-[EFW]-[A-Z]+,[^\r\n]*', pexpect.TIMEOUT], timeout=180)
    case(f'{who}/connect', p.before + (p.after if i < 2 else b'<TIMEOUT>'))
    if i != 0:
        p.expect(PROMPT, timeout=60)
        return False
    ev(f'{who}: Username: after {time.time()-t0:.1f}s')

    # wrong password first; then the right one (LOGINOUT's retry on the same RTA)
    time.sleep(1); p.send(user + '\r')
    p.expect('Password:', timeout=60); time.sleep(1); p.send(BAD_PW + '\r')
    t1 = time.time()
    i = p.expect([USERP, r'%REM-S-END[^\r\n]*', pexpect.TIMEOUT], timeout=90)
    case(f'{who}/badpw', p.before + (p.after if i < 2 else b'<TIMEOUT 90s>'))
    ev(f'{who}: after the bad password: {["Username:", "%REM-S-END", "nothing"][i]} after {time.time()-t1:.1f}s')
    if i == 1:
        p.expect(PROMPT, timeout=60)
        p.send(f'SET HOST {target}\r'); p.expect(USERP, timeout=180)
    elif i == 2:
        # the session sits in the host's retry read: RETURN asks for Username: again
        p.send('\r'); p.expect(USERP, timeout=60)
    time.sleep(1); p.send(user + '\r')
    p.expect('Password:', timeout=60); time.sleep(1); p.send(pw + '\r')
    p.expect(PROMPT, timeout=180)
    case(f'{who}/login', p.before)
    time.sleep(2)

    case(f'{who}/node', sh(p, 'WRITE SYS$OUTPUT F$GETSYI("NODENAME")," ",F$GETDVI("TT","FULLDEVNAM")'))
    # an 80-character line arriving at once (a paste) at the remote prompt
    p.send('WRITE SYS$OUTPUT "PASTE=",F$GETSYI("NODENAME")," TT=",F$GETDVI("TT","FULLDEVNAM")\r')
    i = p.expect([PROMPT, pexpect.TIMEOUT], timeout=30)
    case(f'{who}/paste80', p.before + (b'' if i == 0 else b'<no prompt within 30 s>'))
    if i == 1:
        p.send('\x15\r'); p.expect([PROMPT, pexpect.TIMEOUT], timeout=30)
        case(f'{who}/paste80-recover', p.before)
    case(f'{who}/show-terminal', sh(p, 'SHOW TERMINAL'))
    case(f'{who}/show-system', sh(p, 'SHOW SYSTEM'))
    case(f'{who}/show-users', sh(p, 'SHOW USERS'))
    case(f'{who}/directory', sh(p, 'DIRECTORY SYS$SYSTEM:LOGINOUT.EXE,DCL.EXE'))
    if inbound_check:
        case(f'{who}/{inbound_check[0]}', sh(p, inbound_check[1]))

    # line editing: DELETE erases the X, CTRL/U kills a whole line
    p.send('SHOW TIMX\x7fE\r'); p.expect(PROMPT, timeout=60)
    case(f'{who}/edit-delete', p.before)
    p.send('SHOW NONSENSE\x15SHOW TIME\r'); p.expect(PROMPT, timeout=60)
    case(f'{who}/edit-ctrlu', p.before)
    # command recall: up-arrow then RETURN repeats SHOW TIME
    p.send('\x1b[A\r'); p.expect(PROMPT, timeout=60)
    case(f'{who}/edit-recall', p.before)

    # CTRL/Y and CTRL/C during a long remote command: the REMOTE command is
    # interrupted and the session goes on (we ask which node answers next)
    for key, name in (('\x19', 'ctrly'), ('\x03', 'ctrlc')):
        p.send('SEARCH SYS$SYSTEM:*.EXE "ZZZZQQQQ"\r')
        time.sleep(4)
        p.send(key)
        i = p.expect([PROMPT, pexpect.TIMEOUT], timeout=90)
        case(f'{who}/{name}', p.before if i == 0 else p.before + b'<TIMEOUT>')
        time.sleep(1)
        if i == 1:
            p.send('\r'); p.expect([PROMPT, pexpect.TIMEOUT], timeout=30)
        case(f'{who}/{name}-after', sh(p, 'WRITE SYS$OUTPUT "NODE=",F$GETSYI("NODENAME")'))

    # CTRL/Y at an idle remote prompt (no command running)
    p.send('\x19')
    i = p.expect([PROMPT, pexpect.TIMEOUT], timeout=10)
    case(f'{who}/ctrly-idle', p.before + (b'' if i == 0 else b'<nothing within 10 s>'))
    if i == 1:
        p.send('\r'); p.expect([PROMPT, pexpect.TIMEOUT], timeout=30)
        case(f'{who}/ctrly-idle-then-return', p.before)
    time.sleep(1)
    case(f'{who}/ctrly-idle-after', sh(p, 'WRITE SYS$OUTPUT "NODE=",F$GETSYI("NODENAME")'))

    p.send('LOGOUT\r')
    i = p.expect([r'%REM-S-END[^\r\n]*', pexpect.TIMEOUT], timeout=120)
    txt = p.before + (p.after if i == 0 else b'<TIMEOUT>')
    p.expect(PROMPT, timeout=60)
    case(f'{who}/logout', txt + p.before)
    case(f'{who}/status', sh(p, 'SHOW SYMBOL $STATUS'))
    case(f'{who}/local-node', sh(p, 'WRITE SYS$OUTPUT "NODE=",F$GETSYI("NODENAME")'))
    return True


def errors(p, who, cmds):
    for c in cmds:
        case(f'{who}/{c}', sh(p, c, t=180))
        case(f'{who}/{c}/status', sh(p, 'SHOW SYMBOL $STATUS'))


def decnetd_proof(v):
    """SET HOST with SYS$SYSTEM: (for this process) a directory holding RTPAD.EXE
    and NOTHING else -- no DECNETD.EXE to activate (as the S1 bench did it)."""
    a = boot_ovmx()
    try:
        case('PROOF/copy', a.cmd('COPY SYS$SYSTEM:RTPAD.EXE SYS$LOGIN:RTPAD.EXE', t=60))
        case('PROOF/define', a.cmd('DEFINE/PROCESS SYS$SYSTEM SYS$LOGIN:', t=30))
        case('PROOF/dir', a.cmd('DIRECTORY/SIZE SYS$SYSTEM:DECNETD.EXE,RTPAD.EXE', t=60))
        case('PROOF/control', a.cmd('RUN SYS$SYSTEM:DECNETD.EXE', t=60))
        case('PROOF/control/status', a.cmd('SHOW SYMBOL $STATUS'))
        p = a.p
        p.send('SET HOST VAXC\r')
        i = p.expect([USERP, r'%[A-Z]+-[EFW]-[A-Z]+,[^\r\n]*', pexpect.TIMEOUT], timeout=120)
        case('PROOF/connect', p.before + (p.after if i < 2 else b'<TIMEOUT>'))
        if i == 0:
            time.sleep(1); p.send('SYSTEM\r'); p.expect('Password:', timeout=60)
            time.sleep(1); p.send(VAX_PW + '\r'); p.expect(PROMPT, timeout=120)
            time.sleep(1)
            case('PROOF/remote', sh(p, 'WRITE SYS$OUTPUT "NODE=",F$GETSYI("NODENAME")'))
            p.send('LOGOUT\r'); p.expect(r'%REM-S-END[^\r\n]*', timeout=120)
            txt = p.before + p.after
            p.expect(PROMPT, timeout=60)
            case('PROOF/logout', txt)
            case('PROOF/status', sh(p, 'SHOW SYMBOL $STATUS'))
        case('PROOF/deassign', a.cmd('DEASSIGN/PROCESS SYS$SYSTEM', t=30))
        case('PROOF/dir-after', a.cmd('DIRECTORY/SIZE SYS$SYSTEM:DECNETD.EXE', t=60))
    finally:
        a.close()


def main():
    relay = subprocess.Popen([sys.executable, os.path.join(HERE, 'udp2mcast.py'), MCAST_G, str(MCAST_P),
                              '41081', '41082'], stdout=open(os.path.join(OUT, 'relay.log'), 'a'),
                             stderr=subprocess.STDOUT)
    v = Vax(); a = None
    try:
        v.boot(); ev('VAXC at Username:'); v.login(); ev('VAXC logged in')
        v.license(); ev('VAXC licensed')
        v.cmd('@SYS$STARTUP:STARTNET', t=300)
        for _ in range(60):
            if re.search(r'State\s*=\s*on', v.cmd('MCR NCP SHOW EXECUTOR')):
                break
            time.sleep(5)
        else:
            raise RuntimeError('VAXC: DECnet executor never reached state on')
        ev('VAXC DECnet executor on')
        v.p.send('SET TERMINAL/DEVICE=VT100\r'); v.p.expect(r'\$ $', timeout=60); time.sleep(1)
        v.cmd('')
        v.cmd('REPLY/DISABLE')   # OPA0 is an operator console: keep OPCOM alarms off the transcript

        if os.environ.get('PROOF_ONLY'):
            decnetd_proof(v)
            ev('DONE')
            return
        # ---- the oracle: VAX -> VAX (loopback through VAXC's own DECnet) ----
        errors(v.p, 'VAX', ['SET HOST NOSUCH', 'SET HOST VAXC /BOGUS'])
        remote_session(v.p, 'VAX-SETHOST-0', '0', 'SYSTEM', VAX_PW, 'VAXC')

        a = boot_ovmx()
        a.cmd('SET TERMINAL/DEVICE=VT100', t=30)
        # ---- DECNETD.EXE out of reach for the whole OVMX outbound run ----
        case('OVMX/decnetd-before', a.cmd('DIRECTORY/SIZE SYS$SYSTEM:DECNETD.EXE,RTPAD.EXE', t=60))
        case('OVMX/decnetd-rename', a.cmd('RENAME SYS$SYSTEM:DECNETD.EXE SYS$SYSTEM:DECNETD.S2HIDDEN', t=60))
        case('OVMX/decnetd-rename/status', a.cmd('SHOW SYMBOL $STATUS'))
        case('OVMX/decnetd-rename2', a.cmd('RENAME SYS$SYSROOT:[SYSEXE]DECNETD.EXE SYS$SYSROOT:[SYSEXE]DECNETD.S2HIDDEN', t=60))
        case('OVMX/decnetd-rename2/status', a.cmd('SHOW SYMBOL $STATUS'))
        case('OVMX/decnetd-after', a.cmd('DIRECTORY/SIZE SYS$SYSTEM:DECNETD.*,RTPAD.EXE', t=60))
        case('OVMX/decnetd-control', a.cmd('RUN SYS$SYSTEM:DECNETD.EXE', t=60))
        case('OVMX/decnetd-control/status', a.cmd('SHOW SYMBOL $STATUS'))

        errors(a.p, 'OVMX', ['SET HOST NOSUCH', 'SET HOST VAXC /BOGUS'])
        remote_session(a.p, 'OVMX-SETHOST-VAXC', 'VAXC', 'SYSTEM', VAX_PW, 'OVMXA')
        remote_session(a.p, 'OVMX-SETHOST-0', '0', 'SYSTEM', OVMX_PW, 'OVMXA')
        case('OVMX/decnetd-still-hidden', a.cmd('DIRECTORY/SIZE SYS$SYSTEM:DECNETD.*', t=60))
        case('OVMX/decnetd-restore', a.cmd('RENAME SYS$SYSROOT:[SYSEXE]DECNETD.S2HIDDEN SYS$SYSROOT:[SYSEXE]DECNETD.EXE', t=60))

        # ---- inbound: VAX -> OVMX (the VAX client's CTRL/Y etc. on the wire) ----
        remote_session(v.p, 'VAX-SETHOST-OVMXA', 'OVMXA', 'SYSTEM', OVMX_PW, 'VAXC')
        v.cmd('MCR NCP SHOW EXECUTOR COUNTERS')
        ev('DONE')
    finally:
        if a: a.close()
        subprocess.run(['docker', 'rm', '-f', CTR], capture_output=True)
        relay.terminate()


if __name__ == '__main__':
    main()
