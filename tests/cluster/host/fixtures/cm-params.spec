%OVMX-CLUSTER-SPECIMEN-1
name:      cm-params
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(j) "VOTES -- GROUNDED across four configurations"
capture:   formation-ci1-joinwindow.pcap
wire-len:  204
sha256:    e9a249bdf6071a92f081a608d590b564a6d932ef6a5832c56b87a9c0703935a1
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
; cat 0x01 op 0x01 -- cluster-parameters: VOTES at body[22:24] (GROUNDED
; byte-exact across all four observed vote values {0,1,2}; a non-voting
; OVMX node sends 0x0000, spec 4(j)) plus the node-parameter block that
; the cat-0x06 close (see cm-close-resp.spec) reuses verbatim.
;
@12   60 07
@14   bc 00
@30   4b
@31   13
@72   03 00                      ; body0  send-msg#
@74   01 00                      ; body2  ack-msg#
@76   00 00                      ; body4  txn (0 on the joiner's own config)
@78   00 00                      ; body6  token
@80   01                         ; body8  category 0x01
@81   01                         ; body9  opcode 0x01 (cluster parameters)
@94   00 00                      ; body22:24 VOTES = 0 (non-voting, sec 4(j))
@144  10 00 00 00                ; body72:76 node-param field1 (observed const)
@148  01 00 00 00                ; body76:80 node-param field2 (observed const)
@160  56 37 2e 33 20 20 20 20    ; body88:96 "V7.3    "
