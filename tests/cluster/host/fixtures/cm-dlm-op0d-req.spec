%OVMX-CLUSTER-SPECIMEN-1
name:      cm-dlm-op0d-req
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(p) "Request layout (GROUNDED)"
capture:   scs-dlm-lockconflict.pcap
wire-len:  204
sha256:    fdee2816f60c79a0d7e0554e125dc999a10089b98c85b11532b6d4a5832b5dff
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
; cat 0x02 op 0x0d -- the DLM lock-resource rebuild record replayed
; during the barrier (spec 4(p): "the ONLY cat-0x02 opcode that occurs
; during a join"). body[12:14]/body[14:16] are the two invariant tags;
; body[47] is the resource-NAME length, body[48:48+len] the name itself,
; in the documented Files-11 namespace ("F11B$a..." sec 4(f)). body[34]
; (the result-code stamp) is deliberately 0x00 here -- one of the several
; values spec 4(p) says a REQUEST may carry -- to prove the response
; forces 0xf9 unconditionally.
;
@12   60 07
@14   bc 00
@30   4b
@31   13
@72   07 00                      ; body0  send-msg#
@74   03 00                      ; body2  ack-msg#
@76   15 00                      ; body4  txn
@78   99 00                      ; body6  token
@80   02                         ; body8  category 0x02
@81   0d                         ; body9  opcode 0x0d
@84   01 00                      ; body12:14 invariant tag 0x0001
@86   03 00                      ; body14:16 invariant tag 0x0003
@88   10                         ; body16 L1 length
@106  00                         ; body34 result stamp on the REQUEST side
@119  0d                         ; body47 resource-name length = 13
@120  46 31 31 42 24 61 53 59 53 44 53 4b 31
                                 ; body48.. "F11B$aSYSDSK1" (spec 4(f)
                                 ; Files-11 volume-lock namespace)
