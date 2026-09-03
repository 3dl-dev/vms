%OVMX-CLUSTER-SPECIMEN-1
name:      cm-close-req
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(p) "cat 0x06 -- closes the transaction"
capture:   formation-ci1-joinwindow.pcap
wire-len:  204
sha256:    61f6fdd04a59ce3450662ee37ddb10b4155b47a512d22ef515f677c9c4789be4
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
; cat 0x06 op 0x00 -- the transaction-close / recurring member poll (spec
; 4(p)/(q)). body[20:24] (abs 92) carries a FABRICATED value standing in
; for "the peer's live Con.ID / cluster id" spec 4(p) warns this category
; carries -- echoing it back is what bugchecked a real VAX with
; INCONSTATE. cm-close-resp.spec proves OVMX's builder does NOT reflect
; it: this is the honesty assertion the recipe exists for.
;
@12   60 07
@14   bc 00
@30   4b
@31   13
@72   01 00                      ; body0  send-msg# (the peer's own)
@74   00 00                      ; body2  ack-msg#
@76   09 00                      ; body4  txn
@78   34 12                      ; body6  token
@80   06                         ; body8  category 0x06
@81   00                         ; body9  opcode 0x00
@92   ef be ad de                ; body20:24 stand-in for the peer's LIVE
                                 ; Con.ID/cluster-id data -- MUST NOT appear
                                 ; in the response (see cm-close-resp.spec)
