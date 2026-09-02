%OVMX-CLUSTER-SPECIMEN-1
name:      cm-op0f-resp
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(r) "the 0x0f row... neither is a node setting the byte"
capture:   formation-ci1-joinwindow.pcap
wire-len:  204
sha256:    f0f38c9cc013dafc0bab36549dd5d5b9237b5230c0fdfdcd9fbf9ea8e8815941
%bytes
; Full 32-byte SCS envelope prefix (spec sec 4(d)) -- required so this
; fixture classifies as 'scs-msg' and satisfies the shared harvest-span
; round-trip test (tests/cluster/host/test_codec_roundtrip.c). This item's
; own byte-exactness assertions (test_codec_cm.c) are scoped to the BODY
; span only -- see the file header of vms_cluster_codec_cm.h sec 3 for why
; the envelope span is not this item's grounded scope.
@0    08 00 2b 78 56 b9          ; eth dst (the response direction: VAX2->VAX1)
@6    08 00 2b 4a b7 15          ; eth src
@12   60 07                      ; ethertype 0x6007
@14   bc 00                      ; SCA content 190 (spec 4(d))
@16   aa 00 04 00 02 04          ; dst logical
@22   01 00                      ; connect flag
@24   aa 00 04 00 01 04          ; src logical
@30   4b                         ; msgtype
@31   13                         ; format constant
;
; The response to cm-op0f-req.spec: body[18] stays 0x00 -- ECHOED, not
; forced to 0x01 the way every other cat-0x01 opcode in this recipe is.
; This is the whole point of the 0x0f row existing separately.
;
@72   40 00                      ; body0  OUR OWN send-msg#
@74   05 00                      ; body2  OUR OWN ack-msg#
@76   02 00                      ; body4  txn -- ECHOED
@78   03 00                      ; body6  token -- ECHOED
@80   81                         ; body8  0x01 | 0x80
@81   0f                         ; body9  opcode -- ECHOED
@88   30                         ; body16 role -- ECHOED
@89   03                         ; body17 class -- ECHOED
@90   00                         ; body18 STILL 0x00 -- the decisive assertion
