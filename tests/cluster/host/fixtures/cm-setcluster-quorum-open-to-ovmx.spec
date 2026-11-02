%OVMX-CLUSTER-SPECIMEN-1
name:      cm-setcluster-quorum-open-to-ovmx
class:     scs-msg
origin:    capture
capture:   labc350-cm.pcap
spec:      docs/cluster-protocol-spec.md 4(r) (rd vms-c350)
wire-len:  204
sha256:    5796395174722bb7765900f460b775385ecebbb6ef69f20ee1c7d032ab672b50
%bytes
; HELD-OUT, REAL captured frame 290 of the trimmed labc350 run (2026-11-02, fixed build 882558798): VAX1 SET
; CLUSTER/EXPECTED_VOTES=7 (VAX1 printed 'new value of expected votes is 7, yields quorum of 4'), class-0x04
; op 0x0d to OVMXE (VOTES 3). body[22:24] = 4 -- the QUORUM, not the 7. VAX2 then read CLUSTER_QUORUM=4.
@0    52 54 00 00 e5 01 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 06 04 01 00 aa 00 04 00 01 04 4b 13
@32   33 00 36 00 01 00 12 00 33 00 00 00 36 00 00 00
@48   33 00 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   05 00 cb 68 0d 00 52 2f 07 00 06 00 08 00 3a 21
@80   01 0d 00 00 2b 00 0e 03 40 04 00 00 04 00 04 00
@96   00 4e 00 00 01 01 00 00 00 95 25 97 0c 4e bc 00
@112  00 39 8e 00 0f 4e bc 00 00 01 04 00 00 00 00 0e
@128  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 03
@160  00 00 00 07 00 30 2e 37 03 00 00 00 00 00 00 00
@176  00 00 00 60 ee 78 de ff ff ff 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 00 00 00 00
