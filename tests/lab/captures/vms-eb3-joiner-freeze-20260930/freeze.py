#!/usr/bin/env python3
# freeze.py <bridge> <joiner-sysid> <pid> <match> <secs> <log>
#   match: "0a"    -> the coordinator's cat-0x01 op-0x0a (barrier GO) TO the joiner
#          "8109"  -> the joiner's own cat-0x81 op-0x09 answer (freeze BEFORE the GO lands)
# SIGSTOPs the joiner's SIMH process on the first matching frame, SIGCONTs it after
# <secs>. Nothing is injected on the wire and no frame is altered.
import os, signal, socket, struct, sys, time
br, jsid, pid, match, secs, log = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], float(sys.argv[5]), sys.argv[6]
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
s.bind((br, 0))
PACKET_ADD_MEMBERSHIP=1; PACKET_MR_PROMISC=1
s.setsockopt(263, PACKET_ADD_MEMBERSHIP, struct.pack("IHH8s", socket.if_nametoindex(br), PACKET_MR_PROMISC, 0, b""))
mac = {}
def say(m):
    with open(log, 'a') as f: f.write('%.6f %s\n' % (time.time(), m))
DROP = os.environ.get('DROP_GO') == '1'
import subprocess
def sh(c): return subprocess.run(c, shell=True, capture_output=True, text=True).stdout
if DROP:
    sh('tc qdisc del dev %s root 2>/dev/null' % br)
    sh('tc qdisc add dev %s root handle 1: prio' % br)
    sh('tc filter add dev %s parent 1: protocol all prio 1 u32 match u16 0x010a 0xffff at 66 action drop' % br)
    say('DROP_GO armed on %s' % br)
say('armed match=%s joiner=%d pid=%d secs=%s' % (match, jsid, pid, secs))
while True:
    p = s.recv(4096)
    if len(p) < 82 or p[12:14] != b"\x60\x07":
        continue
    src = p[28] | (p[29] << 8)
    if src: mac.setdefault(bytes(p[6:12]), src)
    if p[30] not in (0x4b, 0x5b): continue
    if struct.unpack('<H', p[60:62])[0] != 10: continue
    cat, op = p[80], p[81]
    dst = mac.get(bytes(p[0:6]))
    hit = (match == '0a' and cat == 0x01 and op == 0x0a and dst == jsid) or \
          (match == '8109' and cat == 0x81 and op == 0x09 and src == jsid)
    if hit:
        os.kill(pid, signal.SIGSTOP)
        say('FREEZE on cat=%02x op=%02x src=%d dst=%s' % (cat, op, src, dst))
        if DROP:
            time.sleep(0.5)
            st = sh('tc -s filter show dev %s' % br)
            sh('tc qdisc del dev %s root' % br)
            say('DROP_GO removed: ' + ' '.join(l.strip() for l in st.splitlines() if 'Sent' in l))
            time.sleep(secs - 0.5)
        else:
            time.sleep(secs)
        os.kill(pid, signal.SIGCONT)
        say('THAW after %ss' % secs)
        break
