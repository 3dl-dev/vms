#!/usr/bin/env python3
"""dapprobe.py - DECnet Phase IV NSP + DAP lab instrument (rd vms-a8a).

A clean-room RE instrument, NOT product code: speaks NSP + DAP to a real
OpenVMS FAL (object 17) over AF_PACKET so the DAP framing OVMX's C codec must
match can be measured against a live VMS. Wire formats from the public DAP 5.6
spec (AA-K177A-TK) + docs/oracle/vax-copy-fal-dap.* captures.

usage: dapprobe.py IFACE MYADDR PEERADDR USER PW get REMOTESPEC
       dapprobe.py IFACE MYADDR PEERADDR USER PW put REMOTESPEC LINE [LINE...]
"""
import os, socket, struct, sys, threading, time, select

ETH_P = 0x6003
def addr16(a):
    ar, nd = a.split('.')
    return (int(ar) << 10) | int(nd)
def mac(a16):
    return bytes([0xaa, 0x00, 0x04, 0x00, a16 & 0xff, a16 >> 8])

IFACE, ME, PEER = sys.argv[1], addr16(sys.argv[2]), addr16(sys.argv[3])
USER, PW, MODE, RSPEC = sys.argv[4], sys.argv[5], sys.argv[6], sys.argv[7]
LINES = sys.argv[8:]
VERBOSE = os.environ.get('V', '1') == '1'
SYSCAP_HEX = os.environ.get('SYSCAP', '')   # override, hex bytes of EX field
VER = bytes.fromhex(os.environ.get('VER', '0506000000'))
TRAILER = bytes.fromhex(os.environ.get('TRAILER', ''))  # append to each data seg (experiment)

s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P))
s.bind((IFACE, ETH_P))
MYMAC, PEERMAC = mac(ME), mac(PEER)

def tx(dst, payload):
    body = struct.pack('<H', len(payload)) + payload
    fr = dst + MYMAC + struct.pack('>H', ETH_P) + body
    if len(fr) < 60: fr += b'\0' * (60 - len(fr))
    s.send(fr)

def hello():
    p = bytes([0x0d, 2, 0, 0]) + MYMAC + bytes([3]) + struct.pack('<H', 1498) + b'\0' \
        + b'\0' * 8 + bytes([0xaa, 0, 4, 0, 0, 0]) + struct.pack('<H', 15) + b'\0' + bytes([2, 0xaa, 0xaa])
    tx(bytes([0xab, 0, 0, 3, 0, 0]), p)

def router_hello():
    p = bytes([0x0b, 2, 0, 0]) + MYMAC + bytes([2]) + struct.pack('<H', 1498) + bytes([0x40, 0]) \
        + struct.pack('<H', 15) + b'\0' + bytes([8]) + b'\0' * 7 + b'\0'
    tx(bytes([0xab, 0, 0, 4, 0, 0]), p)

def hello_loop():
    while True:
        if os.environ.get('ROUTER'): router_hello()
        else: hello()
        time.sleep(10)

def route(nsp, flags=0x26):
    hdr = bytes([0x81, flags, 0, 0]) + mac(PEER) + bytes([0, 0]) + MYMAC + bytes([0, 0, 0, 0])
    return hdr + nsp

def log(*a):
    if VERBOSE: print(time.strftime('%H:%M:%S'), *a, flush=True)

LL = 0x4000 | (os.getpid() & 0x0fff)
peer_ll = None
tx_seg = 0
rx_seg = 0
rxq = []

def rx_nsp(timeout):
    """Return next NSP message (bytes) addressed to us from PEER, or None."""
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([s], [], [], max(0, end - time.time()))
        if not r: return None
        fr = s.recv(2000)
        if fr[0:6] != MYMAC or fr[12:14] != b'\x60\x03': continue
        L = struct.unpack('<H', fr[14:16])[0]
        p = fr[16:16 + L]
        i = 0
        if p[0] & 0x80: i = p[0] & 0x7f
        flg = p[i]
        if (flg & 0x07) != 0x06: continue  # long data only
        if struct.unpack('<H', p[i + 15:i + 17])[0] != PEER: continue
        m = p[i + 21:]
        if len(m) >= 3 and m[0] != 0x08 and m[0] != 0x18 and struct.unpack('<H', m[1:3])[0] != LL: continue
        return m
    return None

def split_msgs(buf):
    out = []; i = 0
    while i < len(buf):
        op = buf[i]; fl = buf[i + 1]; j = i + 2
        if fl & 0x01: j += 1
        ln = None
        if fl & 0x02:
            ln = buf[j]; j += 1
            if fl & 0x04: ln |= buf[j] << 8; j += 1
        if fl & 0x08: j += 1
        if fl & 0x20: j += 1 + buf[j]
        body = buf[j:j + ln] if ln is not None else buf[j:]
        out.append((op, fl, body))
        i = j + len(body)
    return out

pending = []
def next_msg(timeout=30):
    while not pending:
        d = recv_dap(timeout)
        if d is None: return None
        pending.extend(split_msgs(d))
    return pending.pop(0)

def send_data(dap):
    global tx_seg
    tx_seg += 1
    nsp = bytes([0x60]) + struct.pack('<HHHH', peer_ll, LL, 0x8000 | rx_seg, tx_seg) + dap + TRAILER
    tx(PEERMAC, route(nsp))
    log('TX DAP', dap.hex())

def send_ack():
    tx(PEERMAC, route(bytes([0x04]) + struct.pack('<HHH', peer_ll, LL, 0x8000 | rx_seg)))

def recv_dap(timeout=30):
    """Return next DAP payload; handle acks/link-service transparently."""
    global rx_seg
    end = time.time() + timeout
    while time.time() < end:
        m = rx_nsp(end - time.time())
        if m is None: return None
        mf = m[0]
        if mf in (0x00, 0x20, 0x40, 0x60):
            i = 5
            v = struct.unpack('<H', m[i:i + 2])[0]
            if v & 0x8000:
                i += 2; v = struct.unpack('<H', m[i:i + 2])[0]
                if v & 0x8000: i += 2; v = struct.unpack('<H', m[i:i + 2])[0]
            seg = v & 0x0fff; i += 2
            rx_seg = seg
            dap = m[i:]
            log('RX DAP seg', seg, dap.hex())
            send_ack()
            return dap
        elif mf == 0x10:   # link service: ack it on other-data subchannel
            i = 5
            v = struct.unpack('<H', m[i:i + 2])[0]
            if v & 0x8000: i += 2; v = struct.unpack('<H', m[i:i + 2])[0]
            log('RX link-service seg', v & 0xfff, m.hex())
            tx(PEERMAC, route(bytes([0x14]) + struct.pack('<HHH', peer_ll, LL, 0x8000 | (v & 0xfff))))
        elif mf in (0x04, 0x14):
            pass
        elif mf in (0x38, 0x48):
            log('RX disconnect', m.hex()); return None
        else:
            log('RX nsp', hex(mf), m.hex())
    return None

# ---- DAP encoders (spec 5.6 generic format; no LENGTH -> one msg per segment
#      unless blocked) ----
def ex(v, maxlen):
    out = []
    while True:
        out.append(v & 0x7f); v >>= 7
        if not v: break
    for k in range(len(out) - 1): out[k] |= 0x80
    assert len(out) <= maxlen
    return bytes(out)
def img(b): return bytes([len(b)]) + b
def msg(op, body, length=False):
    if length:
        return bytes([op, 0x02, len(body)]) + body
    return bytes([op, 0x00]) + body
def bits(*bs):
    v = 0
    for b in bs: v |= 1 << b
    return v

def syscap():
    if SYSCAP_HEX: return bytes.fromhex(SYSCAP_HEX)
    return ex(bits(1, 5, 18, 20, 33, 40), 12)

def config():
    return msg(1, struct.pack('<H', 1459) + bytes([7, 3]) + VER + syscap())

def disc():
    if peer_ll is not None:
        tx(PEERMAC, route(bytes([0x38]) + struct.pack('<HHH', peer_ll, LL, 0) + b'\0'))

SERVED = set()

def serve():
    """Wait for an inbound object-17 Connect Initiate, accept it, and pipe the
    link to `faldrv serve` (OVMX's compiled dnet_fal_server_run)."""
    global peer_ll
    print('waiting for inbound CI', flush=True)
    end = time.time() + 600
    while time.time() < end:
        r, _, _ = select.select([s], [], [], 1)
        if not r: continue
        fr = s.recv(2000)
        if fr[0:6] != MYMAC or fr[12:14] != b'\x60\x03': continue
        L = struct.unpack('<H', fr[14:16])[0]; p = fr[16:16 + L]
        i = p[0] & 0x7f if p[0] & 0x80 else 0
        if (p[i] & 0x07) != 0x06: continue
        m = p[i + 21:]
        if m[0] != 0x18: continue
        if struct.unpack('<H', m[3:5])[0] in SERVED: continue   # a retransmitted CI
        peer_ll = struct.unpack('<H', m[3:5])[0]
        SERVED.add(peer_ll)
        log('RX CI from', hex(struct.unpack('<H', p[i + 15:i + 17])[0]), m.hex())
        break
    if peer_ll is None: print('NO CI'); return 2
    cc = bytes([0x28]) + struct.pack('<HH', peer_ll, LL) + bytes([0x01, 0x03]) + struct.pack('<H', 1459) + b'\0'
    tx(PEERMAC, route(cc))
    log('TX CC')
    import subprocess
    ch = subprocess.Popen([os.environ['DRV'], 'serve', os.environ.get('SRVDIR', '/tmp')],
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    while True:
        ln = ch.stdout.readline()
        if not ln: break
        if ln.startswith('S '):
            send_data(bytes.fromhex(ln[2:].strip()))
        elif ln.startswith('R'):
            d = recv_dap(120)
            ch.stdin.write((d.hex() if d is not None else '-') + '\n'); ch.stdin.flush()
        elif ln.startswith('X '):
            print('OVMX FAL SERVER STATUS', ln[2:].strip(), flush=True); break
    ch.wait()
    return 0

def main():
    global peer_ll, tx_seg, rx_seg
    threading.Thread(target=hello_loop, daemon=True).start()
    if MODE == 'serve':
        global peer_ll, tx_seg, rx_seg
        rc = 0
        for _ in range(int(os.environ.get('SESSIONS', '3'))):
            peer_ll = None; tx_seg = 0; rx_seg = 0
            rc = serve()
            if rc: break
        return rc
    time.sleep(2)
    # Session Control connect data: dst fmt0 obj 17; src fmt2 grp/usr + name
    sc = bytes([0x00, 0x11]) + bytes([0x02, 0x00]) + struct.pack('<HH', 0x0001, 0x0004) + img(b'OVMX') \
        + bytes([0x27]) + img(USER.encode()) + img(PW.encode()) + img(b'') + img(b'')
    if os.environ.get('DRV'):
        import subprocess
        sc = bytes.fromhex(subprocess.check_output([os.environ['DRV'], 'ci', USER, PW]).decode().strip())
        log('CI SC data from OVMX builder', sc.hex())
    ci = bytes([0x18]) + struct.pack('<HH', 0, LL) + bytes([0x01, 0x03]) + struct.pack('<H', 1459) + sc
    tx(PEERMAC, route(ci, 0x2e))
    log('TX CI')
    end = time.time() + 60
    while time.time() < end:
        m = rx_nsp(end - time.time())
        if m is None: break
        if m[0] == 0x28:
            peer_ll = struct.unpack('<H', m[3:5])[0]; log('RX CC peer ll', hex(peer_ll), m.hex()); break
        if m[0] in (0x38, 0x48, 0x24):
            log('RX', hex(m[0]), m.hex())
            if m[0] != 0x24: print('CONNECT REFUSED'); return 2
    if peer_ll is None: print('NO CC'); return 2
    # flow control: link service (no change) seg 1
    tx(PEERMAC, route(bytes([0x10]) + struct.pack('<HHHH', peer_ll, LL, 0x8000, 1) + b'\0\0'))
    if os.environ.get('DRV'):
        import subprocess
        args = [os.environ['DRV'], MODE, RSPEC, LINES[0] if LINES else '/tmp/drvlocal.txt']
        ch = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        while True:
            ln = ch.stdout.readline()
            if not ln: break
            if ln.startswith('S '):
                send_data(bytes.fromhex(ln[2:].strip()))
            elif ln.startswith('R'):
                d = recv_dap()
                ch.stdin.write((d.hex() if d is not None else '-') + '\n'); ch.stdin.flush()
            elif ln.startswith('X '):
                print('OVMX FAL CLIENT STATUS', ln[2:].strip()); break
        ch.wait()
        return 0 if ch.returncode == 0 else 4
    send_data(config())
    m = next_msg()
    log('peer CONFIG', m[2].hex() if m else None)
    def until(ops):
        while True:
            m = next_msg()
            if m is None: return None
            log('MSG', m[0], m[1], m[2].hex())
            if m[0] in ops: return m
    if MODE == 'get':
        att = msg(2, ex(bits(0, 1, 2, 3), 6) + ex(bits(0), 2) + bytes([0, 2]) + ex(bits(1), 3), True)
        send_data(att) if os.environ.get('SPLIT') else None
        send_data((b'' if os.environ.get('SPLIT') else att) + msg(3, bytes([1, 0]) + img(RSPEC.encode()) + ex(bits(1), 3) + ex(bits(1), 3) + ex(bits(0, 8), 4)))
        m = until((6, 9))
        if m is None or m[0] == 9: print('ACCESS FAILED', m); return 3
        send_data(msg(4, bytes([2, 0])))                        # CONTROL CONNECT
        m = until((6, 9))
        if m is None or m[0] == 9: print('CONNECT FAILED', m); return 3
        send_data(msg(4, bytes([1]) + ex(bits(0), 4) + bytes([3])))  # CONTROL GET RAC=seq file xfer
        recs = []
        while True:
            m = next_msg()
            if m is None: break
            if m[0] == 8:
                b = m[2]; n = b[0]; recs.append(b[1 + n:])
            else:
                log('MSG', m[0], m[1], m[2].hex())
                if m[0] == 9: break
        send_data(msg(7, bytes([1])))                           # ACCOMP CLOSE
        m = until((7, 9))
        print('CLOSE ->', m)
        print('RECORDS', len(recs))
        for x in recs: print('REC', repr(x))
    else:
        att = msg(2, ex(bits(0, 1, 2, 3), 6) + ex(bits(0), 2) + bytes([0, 2]) + ex(bits(1), 3), True)
        acc = msg(3, bytes([2, 0]) + img(RSPEC.encode()) + ex(bits(0), 3) + ex(0, 3) + ex(bits(0, 8), 4))
        send_data(att + acc)
        m = until((6, 9))
        if m is None or m[0] == 9: print('ACCESS FAILED', m); return 3
        send_data(msg(4, bytes([2, 0])))
        m = until((6, 9))
        if m is None or m[0] == 9: print('CONNECT FAILED', m); return 3
        send_data(msg(4, bytes([4]) + ex(bits(0), 4) + bytes([3])))  # CONTROL PUT RAC=3
        for ln in LINES:
            send_data(msg(8, bytes([0]) + ln.encode()))
        send_data(msg(7, bytes([1])))
        m = until((7, 9))
        print('CLOSE ->', m)
    tx(PEERMAC, route(bytes([0x38]) + struct.pack('<HHH', peer_ll, LL, 0) + b'\0'))
    rx_nsp(3)
    return 0

rc = 1
try:
    rc = main()
finally:
    disc()
sys.exit(rc)
