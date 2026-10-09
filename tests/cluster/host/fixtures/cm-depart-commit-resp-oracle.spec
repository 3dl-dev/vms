%OVMX-CLUSTER-SPECIMEN-1
name:      cm-depart-commit-resp-oracle
class:     scs-msg
origin:    capture
capture:   m4-crashwindow.pcap
spec:      docs/cluster-protocol-spec.md 4(r) (rd vms-e8b)
wire-len:  204
sha256:    7a926446611cb201566626f7cfbfa45aa0b8629b1ea73e3fe1cfaedd1249caee
%bytes
; REAL captured frame 89 (0-based) of the same capture, 226 us after the request above.
; REAL OpenVMS VAX V7.3 VAX2's own cat-0x81 op-0x03 ANSWER to it, on the connection the
; request arrived on. THE ORACLE for the class-0x04 response recipe: the request echoed
; with body[8] |= 0x80 and body[18] = 0x01 -- class 0x04 and body[55] ECHOED, not cleared.
@0    aa 00 04 00 01 04 aa 00 04 00 02 04 60 07 bc 00
@16   aa 00 04 00 01 04 01 00 aa 00 04 00 02 04 4b 13
@32   28 25 51 36 01 00 12 00 28 25 00 00 51 36 00 00
@48   28 25 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   08 00 ef c6 08 00 ef c6 56 35 2d 24 05 00 1f bb
@80   81 03 00 00 2b 00 0e 03 20 04 01 00 80 73 5a 59
@96   f2 3a bc 00 80 ac 95 87 04 00 f9 20 20 20 20 00
@112  00 00 00 00 00 00 00 18 43 41 43 48 45 24 63 6d
@128  53 59 53 44 53 4b 31 20 20 20 20 20 86 01 00 00
@144  09 94 f8 7f 7f 00 00 00 00 00 00 00 00 00 00 00
@160  10 41 00 00 00 00 0b 00 01 00 00 00 99 0f 06 20
@176  00 00 00 00 00 00 00 00 ce 00 00 00 74 65 72 73
@192  00 00 00 00 ff ff ff ff 5e 8c 06 df
