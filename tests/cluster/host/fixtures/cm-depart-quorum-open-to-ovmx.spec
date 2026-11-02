%OVMX-CLUSTER-SPECIMEN-1
name:      cm-depart-quorum-open-to-ovmx
class:     scs-msg
origin:    capture
capture:   c350-removenode-cm.pcap
spec:      docs/cluster-protocol-spec.md 4(r) (rd vms-c350)
wire-len:  204
sha256:    65f743d25691f7aaf87e7e620f28ce472dcb1186d930a972c1b5871025309c4a
%bytes
; REAL captured frame 293 (0-based) of the trimmed vms-93fa run (pod lab93fa, 2026-11-02): VAX1 (1025, VOTES 1)
; leaving by SHUTDOWN/REMOVE_NODE sends OVMXE (1030, VOTES 1, EXPECTED_VOTES 3) its class-0x04 open, cat 0x01
; op 0x0d. body[22:24] = 1: the adjusted quorum. VAX2 (VOTES 0) then read CLUSTER_QUORUM=1 with F$GETSYI.
@0    52 54 00 00 e5 01 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 06 04 01 00 aa 00 04 00 01 04 4b 13
@32   42 00 44 00 01 00 12 00 42 00 00 00 44 00 00 00
@48   42 00 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   06 00 bf 85 0e 00 52 2f 10 00 10 00 09 00 6f 1e
@80   01 0d 00 00 2b 00 0e 03 40 04 00 00 04 00 01 00
@96   00 4e 00 00 01 01 00 00 a0 44 e5 fd 04 4e bc 00
@112  00 ed 4d 1b 08 4e bc 00 00 01 04 00 00 00 00 0e
@128  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 03
@160  00 00 00 01 00 30 2e 37 03 00 00 00 00 00 00 00
@176  00 00 00 60 ee 78 de ff ff ff 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 00 00 00 00
