%OVMX-CLUSTER-SPECIMEN-1
name:      cm-barrier-step
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(p) "step N (LE u32) at body[16:20]"
capture:   ovmx-760-MEMBER-achieved-20260730.pcap
wire-len:  204
sha256:    dfbc54880ef071721120726aa8dcecba194830c437e77096c5ebd24aa7de0869
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
; cat 0x01 op 0x0b -- a joiner-initiated barrier step. body[16:20] is a
; plain LE u32 step index HERE (NOT the role/class byte pair op 0x08/09/0d
; use at the same offset -- spec 4(p)/(r) is explicit these are different
; fields depending on opcode).
;
@12   60 07
@14   bc 00
@30   4b
@31   13
@72   01 00                      ; body0  send-msg#
@74   00 00                      ; body2  ack-msg#
@76   00 00                      ; body4  txn (our own per-VC context id)
@78   00 00                      ; body6  token
@80   01                         ; body8  category 0x01
@81   0b                         ; body9  opcode 0x0b (barrier step)
@84   03 00 00 00                ; body12:16 epoch = 3, LE u32 (copied from
                                 ; the coordinator's transition-open)
@88   05 00 00 00                ; body16:20 step = 5, LE u32
