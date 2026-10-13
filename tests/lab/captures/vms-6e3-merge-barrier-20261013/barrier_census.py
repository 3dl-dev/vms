#!/usr/bin/env python3
# barrier_census.py <capture-root>... -- rd vms-6e3.
#
# For every ADD open (cat 0x01 op 0x09, role 0x40) in the captures, by the
# rebuild type it carries at body[24]: which participants sent an op-0x0b
# barrier step after the GO, what the coordinator's 0x81/0x0b ack carried at
# body[18], and whether the coordinator sent any op-0x0c release (window: 3 s
# after the GO). OVMX senders (52:54:00 MACs) are marked with '*'.
import os, struct, sys, collections


def frames(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\xd4\xc3\xb2\xa1':
        return
    o = 24
    while o + 16 <= len(d):
        ts, tu, cl, _ = struct.unpack('<IIII', d[o:o + 16])
        o += 16
        f = d[o:o + cl]
        o += cl
        if len(f) != 204 or f[12:14] != b'\x60\x07' or f[31] != 0x13 or f[55] != 2:
            continue
        yield ts + tu / 1e6, f


def who(mac):
    return mac.hex()[-4:] + ('*' if mac.hex().startswith('525400') else '')


tally = collections.Counter()
for root in sys.argv[1:]:
    for r, _, ns in os.walk(root):
        for n in sorted(ns):
            if not n.endswith('.pcap'):
                continue
            cur = None
            for t, f in frames(os.path.join(r, n)):
                b = f[72:]
                op = (b[8], b[9])
                if op == (1, 9) and b[16] == 0x40:
                    if cur is None or cur['epoch'] != b[12:16]:
                        cur = dict(epoch=b[12:16], rb=b[24], coord=f[6:12], go=None,
                                   steps=set(), acks=set(), rel=0, name=n)
                    continue
                if cur is None:
                    continue
                if op == (1, 0x0a) and f[6:12] == cur['coord'] and cur['go'] is None:
                    cur['go'] = t
                if cur['go'] is None:
                    continue
                if t - cur['go'] > 3:
                    k = (cur['rb'], bool(cur['steps']), cur['rel'] > 0)
                    tally[k] += 1
                    print('%-28s rebuild %d coord %-6s steps from %-14s acks[18] %-6s releases %d'
                          % (cur['name'][:28], cur['rb'], who(cur['coord']),
                             ','.join(sorted(cur['steps'])) or '-',
                             ','.join(str(x) for x in sorted(cur['acks'])) or '-', cur['rel']))
                    cur = None
                    continue
                if op == (1, 0x0b):
                    cur['steps'].add(who(f[6:12]))
                if op == (0x81, 0x0b) and f[6:12] == cur['coord']:
                    cur['acks'].add(b[18])
                if op == (1, 0x0c) and f[6:12] == cur['coord']:
                    cur['rel'] += 1
print('\n(rebuild, any step sent, any release sent): count')
for k, v in sorted(tally.items()):
    print('  %s: %d' % (k, v))
