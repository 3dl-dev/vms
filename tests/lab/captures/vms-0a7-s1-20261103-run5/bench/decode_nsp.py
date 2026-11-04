#!/usr/bin/env python3
"""Decode the DECnet Phase IV (ethertype 0x6003) frames of qemu filter-dump
pcaps into text: routing header, NSP message type, link addresses, reasons.
Endnode/router hellos are counted, not listed. Known passwords in NSP data are
replaced by XX before anything is printed (rd vms-3106 bench).

usage: decode_nsp.py <file.pcap> [...]   (env MASK=pw1,pw2)"""
import os, struct, sys

MASK = [m.encode() for m in os.environ.get('MASK', ','.join(os.environ.get(k, '') for k in ('VAX_PW', 'OVMX_PW', 'DN_PW', 'BAD_PW'))).split(',') if m]
NODES = {(1, 963): 'OVMXA', (1, 965): 'VAXC', (1, 964): 'OVMXB'}
NSP = {0x18: 'CI', 0x68: 'CI(retrans)', 0x28: 'CC', 0x38: 'DI', 0x48: 'DC',
       0x04: 'DATA-ACK', 0x14: 'OTHER-ACK', 0x24: 'CONN-ACK', 0x08: 'NOP',
       0x10: 'LINK-SVC', 0x30: 'INTERRUPT'}

def node(b):  # 2-byte little-endian DECnet address
    v = b[0] | b[1] << 8
    a, n = v >> 10, v & 0x3ff
    return f'{a}.{n}' + (f'({NODES[(a, n)]})' if (a, n) in NODES else '')

def pcap(path):
    d = open(path, 'rb').read()
    le = d[:4] == b'\xd4\xc3\xb2\xa1'
    e = '<' if le else '>'
    off = 24
    while off + 16 <= len(d):
        ts, us, incl, _ = struct.unpack(e + 'IIII', d[off:off + 16])
        yield ts + us / 1e6, d[off + 16:off + 16 + incl]
        off += 16 + incl

def mask(b):
    for m in MASK:
        b = b.replace(m, b'XX')
    return b

def show(b, n=40):
    b = mask(b)
    asc = ''.join(chr(c) if 32 <= c < 127 else '.' for c in b[:n])
    return f'{b[:n].hex(" ")}{" ..." if len(b) > n else ""}  |{asc}|'

def decode(path):
    hello = 0; t0 = None; prev = (None, None); dup = 0
    print(f'== {os.path.basename(path)}')
    for ts, f in pcap(path):
        if len(f) < 16 or f[12:14] != b'\x60\x03':
            continue
        t0 = t0 if t0 is not None else ts
        # qemu filter-dump records a frame this NIC sends twice (its tx and the
        # socket-mcast loopback of it): drop an identical frame within 5 ms.
        if f == prev[1] and ts - prev[0] < 0.005:
            dup += 1; continue
        prev = (ts, f)
        dlen = f[14] | f[15] << 8
        p = f[16:16 + dlen]
        if p and p[0] & 0x80:
            p = p[(p[0] & 0x7f):]
        if not p:
            continue
        fl = p[0]
        if fl & 0x07 in (0x03, 0x05):  # hellos / init
            hello += 1; continue
        if fl & 0x07 != 0x06:
            print(f'{ts - t0:9.3f} routing flags {fl:#04x} (not long data) len {len(p)}'); continue
        dst, src = node(p[7:9]), node(p[15:17])
        nsp = p[21:]
        if not nsp:
            continue
        mf = nsp[0]
        if mf & 0x0c == 0 and mf & 0x80 == 0:
            kind = 'DATA' if mf & 0x10 == 0 else ('LINK-SVC' if mf == 0x10 else 'INTERRUPT')
        else:
            kind = NSP.get(mf, f'msgflg {mf:#04x}')
        dla = nsp[1] | nsp[2] << 8 if len(nsp) >= 3 else None
        sla = nsp[3] | nsp[4] << 8 if len(nsp) >= 5 else None
        line = f'{ts - t0:9.3f} {src:>12} -> {dst:<12} rflg {fl:#04x} NSP {kind:<12} dstaddr {dla:#06x}'
        if kind not in ('DATA-ACK', 'OTHER-ACK', 'CONN-ACK') and sla is not None:
            line += f' srcaddr {sla:#06x} ({sla})'
        if kind in ('DI', 'DC') and len(nsp) >= 7:
            r = nsp[5] | nsp[6] << 8
            line += f' reason {r}'
        if kind in ('CI', 'CI(retrans)', 'CC'):
            line += f'  services {nsp[5]:#04x} info {nsp[6]:#04x} segsize {nsp[7] | nsp[8] << 8}'
            line += '\n' + ' ' * 12 + 'data ' + show(nsp[9:], int(os.environ.get('SHOWN', '48')))
        elif kind in ('DATA', 'INTERRUPT', 'LINK-SVC'):
            line += '\n' + ' ' * 12 + 'nsp  ' + show(nsp[5:], int(os.environ.get('SHOWN', '32')))
        print(line)
    print(f'   ({hello} hello/routing-control frames not listed; {dup} filter-dump duplicates dropped)')

for path in sys.argv[1:]:
    decode(path)
