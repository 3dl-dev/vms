#!/usr/bin/env python3
"""Bridge one SIMH 'attach xq udp:LPORT:127.0.0.1:RPORT' Ethernet to a qemu
'-netdev socket,mcast=GROUP:PORT' L2 segment (rd vms-05d lab bench only).
Frames are raw Ethernet in both encapsulations. Our own multicast echoes are
dropped by remembering what we sent."""
import socket, struct, select, sys, collections, time
group, gport = sys.argv[1], int(sys.argv[2])
simh_port, my_port = int(sys.argv[3]), int(sys.argv[4])
u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u.bind(('127.0.0.1', my_port))
m = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
m.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
m.bind(('', gport))
m.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, struct.pack('4s4s', socket.inet_aton(group), socket.inet_aton('0.0.0.0')))
m.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
sent = collections.Counter()
n = {'v2q': 0, 'q2v': 0}
last = time.time()
while True:
    r, _, _ = select.select([u, m], [], [], 5)
    for s in r:
        data, addr = s.recvfrom(65536)
        if s is u:
            sent[data] += 1
            m.sendto(data, (group, gport)); n['v2q'] += 1
        else:
            if sent[data]:
                sent[data] -= 1
                if not sent[data]: del sent[data]
                continue
            u.sendto(data, ('127.0.0.1', simh_port)); n['q2v'] += 1
    if time.time() - last > 30:
        print('relay', n, flush=True); last = time.time()
        if len(sent) > 10000: sent.clear()
