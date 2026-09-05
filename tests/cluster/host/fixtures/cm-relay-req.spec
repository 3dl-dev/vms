%OVMX-CLUSTER-SPECIMEN-1
name:      cm-relay-req
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(p), 4(r) "op 0x12 ... body[20:24] = LE u32 copy of the request's body[12:16]"
capture:   formation-ci1-joinwindow.pcap
wire-len:  204
sha256:    205565a76ea77b6c85c5a6f31b299376619fa1d163b794ba2a1e129256cd99eb
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
; cat 0x01 op 0x12 -- the coordinator's relay of a new member to the
; other members (spec 4(p): "Admission is single-coordinator ... which
; relays the new node to the rest (op 0x12)").
;
@12   60 07
@14   bc 00
@30   4b
@31   13
@72   01 00                      ; body0  send-msg#
@74   00 00                      ; body2  ack-msg#
@76   05 00                      ; body4  txn
@78   06 00                      ; body6  token
@80   01                         ; body8  category 0x01
@81   12                         ; body9  opcode 0x12 (relay)
@84   09 00 00 00                ; body12:16 epoch = 9, LE u32
@88   10                         ; body16 role slot 0x10 (ROLE_RELAY)
@89   02                         ; body17 class 0x02 (the RELAYER's own class;
                                 ; not meaningful to the responder)
