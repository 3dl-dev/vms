#!/usr/bin/env python3
# stallpkt.py <tap> <sysid> <match> <stall-s>   (rd vms-eb3)
#
# THE SAME FAULT AS stall.sh -- the node's QEMU process tree SIGSTOPped, then
# SIGCONTed; nothing on the wire touched -- ARMED ON THE WIRE instead of on a
# console line, so it lands in ONE window of the transition every time:
#
#   match 8109 -- the node's own cat-0x81 op-0x09, its Phase-1 answer. The
#                 coordinator's GO (op-0x0a) arrives ~1 ms later, into a
#                 stopped guest: the oracle-F6 / rig-P-3 window.
#   match 0a   -- the coordinator's cat-0x01 op-0x0a GO to the node, seen
#                 leaving the bridge for its tap: the guest stops with the GO
#                 in its queue (oracle F5).
#
# Offsets are the ones cmconn.py decodes (spec 4(d)/(j)): source SCSSYSTEMID
# low word at [28:30], SCS msg type at [60:62] (10 = application message),
# CM category/opcode at [80]/[81]. Prints the lines stall.sh prints, so
# grade3.sh reads it unchanged.
import os, signal, socket, struct, subprocess, sys, time
tap, sysid, match, secs = sys.argv[1], int(sys.argv[2]), sys.argv[3], float(sys.argv[4])

def qemu_pids():
    out = subprocess.run(['ps', '-eo', 'pid,args'], capture_output=True, text=True).stdout
    return [int(l.split()[0]) for l in out.splitlines()
            if 'qemu-system-x86_64' in l and ('ifname=%s,' % tap) in l]

def now(): return time.strftime('%H:%M:%S', time.gmtime()) + '.%03d' % (int(time.time() * 1000) % 1000)

s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
s.bind((tap, 0))
print('STALL: armed on the wire, %s, match %s, node %d' % (tap, match, sysid), flush=True)
mac = {}
deadline = time.time() + 900
while time.time() < deadline:
    p = s.recv(4096)
    if len(p) < 82 or p[12:14] != b'\x60\x07':
        continue
    src = p[28] | (p[29] << 8)
    if src:
        mac.setdefault(bytes(p[6:12]), src)
    if p[30] not in (0x4b, 0x5b) or struct.unpack('<H', p[60:62])[0] != 10:
        continue
    cat, op, dst = p[80], p[81], mac.get(bytes(p[0:6]))
    if (match == '8109' and cat == 0x81 and op == 0x09 and src == sysid) or \
       (match == '0a' and cat == 0x01 and op == 0x0a and dst == sysid):
        pids = qemu_pids()
        for q in pids: os.kill(q, signal.SIGSTOP)
        print('MARKER seen at %s: cat=%02x op=%02x %d->%s' % (now(), cat, op, src, dst), flush=True)
        if not pids:
            print('STALL: no qemu found for %s -- no fault injected' % tap); sys.exit(1)
        print('STALL on  %s pids %s at %s for %ss' % (tap, pids, now(), secs), flush=True)
        time.sleep(secs)
        for q in pids: os.kill(q, signal.SIGCONT)
        print('STALL off %s pids %s at %s after %ss' % (tap, pids, now(), secs), flush=True)
        sys.exit(0)
print('STALL: marker never appeared -- no fault injected'); sys.exit(1)
