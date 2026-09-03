%OVMX-CLUSTER-SPECIMEN-1
name:      cm-op0f-req
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(r) "the 0x0f row... neither is a node setting the byte"
capture:   formation-ci1-joinwindow.pcap
wire-len:  204
sha256:    a24dd8597482003b78dd7f4788a39becba6269e1dd9b269d929e82c7daa68c7d
%bytes
; Full 32-byte SCS envelope prefix (spec sec 4(d)) -- required so this
; fixture classifies as 'scs-msg' and satisfies the shared harvest-span
; round-trip test (tests/cluster/host/test_codec_roundtrip.c), even though
; this item's own assertions are scoped to the body (see the file header).
@0    08 00 2b 4a b7 15          ; eth dst (VAX1 HW MAC, spec-composed pair)
@6    08 00 2b 78 56 b9          ; eth src (VAX2 HW MAC)
@16   aa 00 04 00 01 04          ; dst logical (VAX1)
@22   01 00                      ; connect flag
@24   aa 00 04 00 02 04          ; src logical (VAX2)
;
; cat 0x01 op 0x0f -- the class-0x03 REMOVE transition's extra step
; (spec 4(r): role slot 0x30, occurs only inside a class-0x03 removal).
; body[18] is DELIBERATELY 0x00 here (one of the two reconciled censuses,
; spec 4(r): "six that leave it 0") to prove the response ECHOES it
; rather than forcing 0x01.
;
@12   60 07
@14   bc 00
@30   4b
@31   13
@72   05 00                      ; body0  send-msg#
@74   01 00                      ; body2  ack-msg#
@76   02 00                      ; body4  txn
@78   03 00                      ; body6  token
@80   01                         ; body8  category 0x01
@81   0f                         ; body9  opcode 0x0f
@88   30                         ; body16 role slot 0x30 (ROLE_0F)
@89   03                         ; body17 class 0x03 (REMOVE)
@90   00                         ; body18 EXPLICIT 0x00 (the echo-not-force proof)
