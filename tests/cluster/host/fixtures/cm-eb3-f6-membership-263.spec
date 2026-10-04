%OVMX-CLUSTER-SPECIMEN-1
name:      cm-eb3-f6-membership-263
class:     scs-msg
origin:    capture
capture:   eb3-F6-20260930.pcap
spec:      docs/cluster-protocol-spec.md 4(j)/4(p) + rd vms-eb3 oracle F5/F6
wire-len:  204
sha256:    3c49469e73406c494e5b3f3d2cfe30fc7e6a9c82db5db22eb33cd20bec9e102d
%bytes
; REAL captured frame 470 of tests/lab/captures/vms-eb3-joiner-freeze-20260930/eb3-F6-20260930.pcap
; VAX3 (1027, the coordinator) to the joiner VAX2 (1026): its last cat-0x01 op-0x06
; membership record before the Phase-1 open -- the joiner has taken 263 when the open arrives.
@0    08 00 2b d7 8e 3f 08 00 2b c4 ad 97 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 03 04 4b 13
@32   78 00 24 01 01 00 12 00 78 00 00 00 24 01 00 00
@48   78 00 00 00 01 00 00 02 92 00 04 00 0a 00 00 00
@64   0a 00 1b 64 0d 00 52 2f 07 01 5a 00 00 00 00 00
@80   01 06 49 53 04 00 00 00 20 02 00 00 00 00 00 00
@96   00 00 00 04 cd fd 4a 9c 4b 34 bc 00 03 00 01 00
@112  02 00 00 00 00 00 01 1a 52 4d 53 24 d6 01 01 00
@128  00 00 02 53 59 53 44 53 4b 31 20 20 20 20 20 00
@144  00 00 f8 7f 70 00 00 00 c0 cc f8 7f 00 00 00 00
@160  00 41 00 00 00 00 4f 0f 01 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 94 dc 24 46
