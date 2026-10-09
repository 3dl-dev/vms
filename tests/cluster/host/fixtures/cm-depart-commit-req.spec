%OVMX-CLUSTER-SPECIMEN-1
name:      cm-depart-commit-req
class:     scs-msg
origin:    capture
capture:   m4-crashwindow.pcap
spec:      docs/cluster-protocol-spec.md 4(r) (rd vms-e8b)
wire-len:  204
sha256:    1ca7d5c07e3a311cf2bf210bdc611ee757f0207a3e6f03225dcc5ae547764062
%bytes
; REAL captured frame 88 (0-based) of tests/lab/captures/vms-e8b-cnxmgrerr-removenode-20261008/m4-crashwindow.pcap
; cat-0x01 op-0x03 role-0x20 CLASS-0x04 membership COMMIT: VAX1 (SCSSYSTEMID 1025) opens
; its OWN departure transition (SHUTDOWN/REMOVE_NODE) toward VAX2 (1026). txn 5, token
; 47903, epoch 51249195. The FIRST class-0x04 op-0x03 specimen in this tree.
@0    aa 00 04 00 02 04 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 4b 13
@32   50 36 28 25 01 00 12 00 50 36 00 00 28 25 00 00
@48   50 36 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   08 00 ef c6 08 00 ef c6 2d 24 55 35 05 00 1f bb
@80   01 03 00 00 2b 00 0e 03 20 04 00 00 80 73 5a 59
@96   f2 3a bc 00 80 ac 95 87 04 00 f9 20 20 20 20 00
@112  00 00 00 00 00 00 00 18 43 41 43 48 45 24 63 6d
@128  53 59 53 44 53 4b 31 20 20 20 20 20 86 01 00 00
@144  09 94 f8 7f 7f 00 00 00 00 00 00 00 00 00 00 00
@160  10 41 00 00 00 00 0b 00 01 00 00 00 99 0f 06 20
@176  00 00 00 00 00 00 00 00 ce 00 00 00 74 65 72 73
@192  00 00 00 00 ff ff ff ff 5e 8c 06 df
