%OVMX-CLUSTER-SPECIMEN-1
name:      cm-0b01-req-older-oracle
class:     scs-msg
origin:    capture
capture:   armB-cm.pcap
spec:      rd vms-52d cat-0x0b op-0x01 startup request / 0x8b answer oracle
wire-len:  204
sha256:    6220f1854d819e35c26d39efbb2af2d6aa2c6b32e52c5c56dbe5987d655a02d8
%bytes
; REAL captured frame 311 of tests/lab/captures/vms-52d-0b01-oracle-20261013/armB-cm.pcap
; arm B: VAX1 (1025), booting, asks VAX3 (1027), the founder in OVMX's exact position
; (coordinator, highest SCSSYSTEMID, LOCKDIRWT 0). VAX3 became a member first.
@0    08 00 2b a3 79 82 08 00 2b f2 e1 0b 60 07 bc 00
@16   aa 00 04 00 03 04 01 00 aa 00 04 00 01 04 4b 13
@32   81 07 c8 07 01 00 12 00 81 07 00 00 c8 07 00 00
@48   81 07 00 00 01 00 00 02 92 00 04 00 0a 00 02 00
@64   0a 00 52 2f 09 00 4f 8e aa 07 61 07 04 00 2e 7f
@80   0b 01 00 00 01 00 02 00 00 00 00 00 51 04 00 03
@96   04 00 00 00 01 00 00 00 01 00 fa 00 00 00 00 00
@112  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@128  53 44 53 4b 31 20 20 20 20 20 18 03 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 01 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff 75 98 38 24
