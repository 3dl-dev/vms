#!/usr/bin/env python3
"""ctermprobe.py - DECnet Phase IV NSP lab instrument for the CTERM HOST role
(rd vms-a70 direction B). LAB HARNESS ONLY, not product code.

Derived from dapprobe.py (tests/lab/captures/decnet-fal-inbound-20261004): it
answers ONE inbound Connect Initiate to object 42 (CTERM) at node MYADDR and
carries that NSP logical link, duplex, for `ctermdrv` -- which runs OVMX's
compiled CTERM host FSM. NSP framing from the public NSP spec + the captures.

usage (root, in the lab pod):  ROUTER=1 DRV=/tmp/ctermdrv ctermprobe.py IFACE 1.44
"""
import os, socket, struct, sys, threading, time, select, subprocess

ETH_P = 0x6003
def addr16(a):
    ar, nd = a.split('.')
    return (int(ar) << 10) | int(nd)
def mac(a16):
    return bytes([0xaa, 0x00, 0x04, 0x00, a16 & 0xff, a16 >> 8])

IFACE, ME = sys.argv[1], addr16(sys.argv[2])
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P))
s.bind((IFACE, ETH_P))
MYMAC = mac(ME)
PEER = None; PEERMAC = None

def log(*a): print(time.strftime('%H:%M:%S'), *a, flush=True)

def tx(dst, payload):
    body = struct.pack('<H', len(payload)) + payload
    fr = dst + MYMAC + struct.pack('>H', ETH_P) + body
    if len(fr) < 60: fr += b'\0' * (60 - len(fr))
    s.send(fr)

def router_hello():
    p = bytes([0x0b, 2, 0, 0]) + MYMAC + bytes([2]) + struct.pack('<H', 1498) + bytes([0x40, 0]) \
        + struct.pack('<H', 15) + b'\0' + bytes([8]) + b'\0' * 7 + b'\0'
    tx(bytes([0xab, 0, 0, 4, 0, 0]), p)

def endnode_hello():
    p = bytes([0x0d, 2, 0, 0]) + MYMAC + bytes([3]) + struct.pack('<H', 1498) + b'\0' \
        + b'\0' * 8 + bytes([0xaa, 0, 4, 0, 0, 0]) + struct.pack('<H', 15) + b'\0' + bytes([2, 0xaa, 0xaa])
    tx(bytes([0xab, 0, 0, 3, 0, 0]), p)

def hello_loop():
    while True:
        router_hello() if os.environ.get('ROUTER') else endnode_hello()
        time.sleep(10)

def route(nsp, flags=0x26):
    return bytes([0x81, flags, 0, 0]) + mac(PEER) + bytes([0, 0]) + MYMAC + bytes([0, 0, 0, 0]) + nsp

LL = 0x4400 | (os.getpid() & 0x00ff)
peer_ll = None; tx_seg = 0; rx_seg = 0

def nsp_in(fr):
    """(src, nsp-bytes) for a long-format routed frame addressed to us."""
    if fr[0:6] != MYMAC or fr[12:14] != b'\x60\x03': return None
    L = struct.unpack('<H', fr[14:16])[0]; p = fr[16:16 + L]
    i = p[0] & 0x7f if p[0] & 0x80 else 0
    if (p[i] & 0x07) != 0x06: return None
    return struct.unpack('<H', p[i + 15:i + 17])[0], p[i + 21:]

def send_data(d):
    global tx_seg
    tx_seg += 1
    nsp = bytes([0x60]) + struct.pack('<HHHH', peer_ll, LL, 0x8000 | rx_seg, tx_seg) + d
    tx(PEERMAC, route(nsp))

def send_ack():
    tx(PEERMAC, route(bytes([0x04]) + struct.pack('<HHH', peer_ll, LL, 0x8000 | rx_seg)))

def main():
    global PEER, PEERMAC, peer_ll, rx_seg
    threading.Thread(target=hello_loop, daemon=True).start()
    log('waiting for an inbound Connect Initiate to object 42 at', sys.argv[2])
    end = time.time() + float(os.environ.get('WAIT', '900'))
    while time.time() < end and peer_ll is None:
        r, _, _ = select.select([s], [], [], 1)
        if not r: continue
        x = nsp_in(s.recv(2000))
        if not x: continue
        src, m = x
        # CI: 18 dst(2) src(2) services info segsize(2) | SC connect: dst fmt0 obj
        if m[0] in (0x18, 0x08) and len(m) > 10 and m[9] == 0x00 and m[10] == 0x2a:
            PEER, PEERMAC = src, mac(src)
            peer_ll = struct.unpack('<H', m[3:5])[0]
            log('RX CI from %d.%d link %04x: %s' % (src >> 10, src & 1023, peer_ll, m.hex()))
    if peer_ll is None: log('NO CI'); return 2
    tx(PEERMAC, route(bytes([0x28]) + struct.pack('<HH', peer_ll, LL) + bytes([0x01, 0x03]) +
                      struct.pack('<H', 1459) + b'\0'))
    log('TX CC')
    ch = subprocess.Popen([os.environ['DRV']] + sys.argv[3:], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, bufsize=0)
    buf = b''
    while True:
        r, _, _ = select.select([s, ch.stdout], [], [], 1)
        if ch.stdout in r:
            d = os.read(ch.stdout.fileno(), 65536)
            if not d: log('driver gone'); break
            buf += d
            done = False
            while b'\n' in buf:
                ln, buf = buf.split(b'\n', 1)
                ln = ln.decode(errors='replace')
                if ln.startswith('S '):
                    seg = bytes.fromhex(ln[2:]); send_data(seg); log('TX seg', tx_seg, seg.hex())
                elif ln.startswith('L '):
                    log('DRV', ln[2:])
                elif ln.startswith('X '):
                    log('DRV END', ln[2:]); done = True
            if done:
                time.sleep(0.2)
                tx(PEERMAC, route(bytes([0x38]) + struct.pack('<HHH', peer_ll, LL, 0) + b'\0'))
                log('TX DI')
                t_end = time.time() + 5
                while time.time() < t_end:
                    rr, _, _ = select.select([s], [], [], 0.5)
                    if rr:
                        x = nsp_in(s.recv(2000))
                        if x and x[1][0] == 0x48: log('RX DC', x[1].hex()); break
                return 0
        if s in r:
            x = nsp_in(s.recv(2000))
            if not x or x[0] != PEER: continue
            m = x[1]
            if len(m) >= 3 and m[0] not in (0x18, 0x08) and struct.unpack('<H', m[1:3])[0] != LL: continue
            mf = m[0]
            if mf in (0x00, 0x20, 0x40, 0x60):
                i = 5
                v = struct.unpack('<H', m[i:i + 2])[0]
                while v & 0x8000:
                    i += 2; v = struct.unpack('<H', m[i:i + 2])[0]
                seg = v & 0x0fff; i += 2
                if seg != ((rx_seg + 1) & 0x0fff):
                    send_ack(); continue          # a retransmission: re-ack, drop
                rx_seg = seg
                d = m[i:]
                log('RX seg', seg, d.hex())
                send_ack()
                ch.stdin.write(b'D ' + d.hex().encode() + b'\n')
            elif mf == 0x10:
                i = 5
                v = struct.unpack('<H', m[i:i + 2])[0]
                if v & 0x8000: i += 2; v = struct.unpack('<H', m[i:i + 2])[0]
                tx(PEERMAC, route(bytes([0x14]) + struct.pack('<HHH', peer_ll, LL, 0x8000 | (v & 0xfff))))
            elif mf in (0x38, 0x48):
                log('RX disconnect', m.hex())
                if mf == 0x38:
                    tx(PEERMAC, route(bytes([0x48]) + struct.pack('<HHH', peer_ll, LL, 42)))
                ch.stdin.write(b'Q\n'); time.sleep(0.2); return 0
    return 1

sys.exit(main())
