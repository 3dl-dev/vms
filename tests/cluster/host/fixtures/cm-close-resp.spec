%OVMX-CLUSTER-SPECIMEN-1
name:      cm-close-resp
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(p) "send your own node-parameter block, the same one carried in the op 0x01 PARAMS message (body[72:76]=0x10, body[76:80]=0x01, body[88:96]=\"V7.3    \")"
capture:   formation-ci1-joinwindow.pcap
wire-len:  204
sha256:    33454b5bae2c5077dac387eb102adddd0a39e711514924f4310a733011e328c8
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
; The response to cm-close-req.spec: token pair carried, our own send/ack,
; response bit, echoed opcode, and OVMX's own node-parameter block --
; built FROM ZERO, never from the request. body[20:24] (abs 92) is
; asserted ALL-ZERO here specifically to prove the fabricated peer data
; in the request was never reflected back (sec 4(p) INCONSTATE warning).
;
@72   02 00                      ; body0  OUR OWN send-msg#
@74   01 00                      ; body2  OUR OWN ack-msg#
@76   09 00                      ; body4  txn -- carried (NOT echoed-from-copy;
                                 ; this is a fresh-built body that ASSERTS it)
@78   34 12                      ; body6  token -- carried
@80   86                         ; body8  0x06 | 0x80
@81   00                         ; body9  opcode -- echoed value (0x00)
@92   00 00 00 00                ; body20:24 ZERO -- the peer's fabricated
                                 ; Con.ID/cluster-id is NOT reflected back
@144  10 00 00 00                ; body72:76 node-param field1 (observed const)
@148  01 00 00 00                ; body76:80 node-param field2 (observed const)
@160  56 37 2e 33 20 20 20 20    ; body88:96 "V7.3    "
