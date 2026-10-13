%OVMX-CLUSTER-SPECIMEN-1
name:      cm-0b01-resp-older-oracle
class:     scs-msg
origin:    capture
capture:   armB-cm.pcap
spec:      rd vms-52d cat-0x0b op-0x01 startup request / 0x8b answer oracle
wire-len:  204
sha256:    f49728c22a2c408b58e2af37b1826f24f971f4b92ac8e3c7674c3319af3ad549
%bytes
; REAL captured frame 312 of tests/lab/captures/vms-52d-0b01-oracle-20261013/armB-cm.pcap
; arm B: VAX3's answer to cm-0b01-req-older-oracle: the echo, body[8] 0x8b, body[28:32] = body[36:40] = 1
@0    08 00 2b f2 e1 0b 08 00 2b a3 79 82 60 07 bc 00
@16   aa 00 04 00 01 04 01 00 aa 00 04 00 03 04 4b 13
@32   c8 07 82 07 01 00 12 00 c8 07 00 00 82 07 00 00
@48   c8 07 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   09 00 4f 8e 0a 00 52 2f 62 07 aa 07 04 00 2e 7f
@80   8b 01 00 00 01 00 02 00 00 00 00 00 51 04 00 03
@96   04 00 00 00 01 00 00 00 01 00 fa 00 01 00 00 00
@112  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@128  53 44 53 4b 31 20 20 20 20 20 18 03 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 01 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff 75 98 38 24
