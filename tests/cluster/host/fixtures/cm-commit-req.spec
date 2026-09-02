%OVMX-CLUSTER-SPECIMEN-1
name:      cm-commit-req
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(j), 4(p), 4(r)
capture:   formation-ci1-joinwindow.pcap
wire-len:  204
sha256:    8737ff376a55d6d5f152adacb3ea75b30080ad7fc8c77e109b1d0ab72c74e93d
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
; cat 0x01 op 0x03 -- the member-driven membership-COMMIT transaction
; (spec 4(j)/(p): "member requests, joiner echoes the token in its 0x81
; response"). body[100] (abs 172) is an arbitrary RESIDUE byte, present to
; prove the echo recipe reproduces bytes it does not itself interpret.
;
@12   60 07
@14   bc 00
@30   4b
@31   13
@72   09 00                      ; body0  send-msg#
@74   02 00                      ; body2  ack-msg#
@76   11 00                      ; body4  txn
@78   22 cc                      ; body6  token
@80   01                         ; body8  category 0x01
@81   03                         ; body9  opcode 0x03 (COMMIT)
@88   20                         ; body16 role slot 0x20 (ROLE_COMMIT)
@89   02                         ; body17 class 0x02 (ADD)
@172  77                         ; body100 (abs 72+100) residue byte
