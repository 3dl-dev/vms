%OVMX-CLUSTER-SPECIMEN-1
name:      cm-eb3-f6-go-resent
class:     scs-msg
origin:    capture
capture:   eb3-F6-20260930.pcap
spec:      docs/cluster-protocol-spec.md 4(j)/4(p) + rd vms-eb3 oracle F5/F6
wire-len:  204
sha256:    f15b95733780c9ea2abbf1c4624e2caae560a0734288ae24a7565488b4f2d0b8
%bytes
; REAL captured frame 557 of tests/lab/captures/vms-eb3-joiner-freeze-20260930/eb3-F6-20260930.pcap
; THE FINDING: the coordinator's GO (cat-0x01 op-0x0a), RE-SENT with its original send-msg# 266 on the connection re-established after the joiner's 14 s freeze.
; The first copy (same number, on the old connection) was dropped at the joiner's tap; the joiner had acked 265 in its accept.
@0    08 00 2b d7 8e 3f 08 00 2b c4 ad 97 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 03 04 4b 13
@32   0b 00 0c 00 02 00 12 00 0b 00 00 00 0c 00 00 00
@48   0b 00 00 00 01 00 00 02 92 00 04 00 0a 00 00 00
@64   0a 00 1e 64 0d 00 5e 2f 0a 01 5e 00 00 00 00 00
@80   01 0a 52 5f 04 00 00 00 60 02 00 00 53 43 53 24
@96   44 49 52 45 43 54 4f 52 59 20 20 20 20 20 20 20
@112  20 20 20 20 20 20 20 20 20 20 20 20 6d 20 75 73
@128  65 72 20 53 59 53 54 45 4d 20 6f 6e 20 56 41 58
@144  31 0d 0a 25 4c 49 43 45 4e 53 45 2d 46 2d 45 58
@160  43 45 45 44 45 44 2c 20 61 74 74 65 6d 70 74 65
@176  64 20 75 73 61 67 65 20 65 78 63 65 65 64 73 20
@192  61 63 74 69 76 65 20 6c 69 63 65 6e
