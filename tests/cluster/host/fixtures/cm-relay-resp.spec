%OVMX-CLUSTER-SPECIMEN-1
name:      cm-relay-resp
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(r) "op 0x12 | body[18] = 0x01; body[17] = the responder's own current class; body[20:24] = LE u32 copy of the request's body[12:16] (the epoch)"
capture:   formation-ci1-joinwindow.pcap
wire-len:  204
sha256:    8558c1f17d10486a60242e786133a9aa7a6e377221d5666aa7c3e36b6d8e6194
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
; The response to cm-relay-req.spec with own_class = 0x04: body[17] is
; OVERWRITTEN with the responder's own class (not echoed, unlike every
; other opcode this file handles) and body[20:24] gets a FRESH LE u32
; copy of the request's epoch -- distinct from body[12:16], which stays
; the plain echo. Both end up holding the same value (9) but from two
; different code paths, which is exactly what this fixture proves.
;
@72   20 00                      ; body0  OUR OWN send-msg#
@74   03 00                      ; body2  OUR OWN ack-msg#
@76   05 00                      ; body4  txn -- ECHOED
@78   06 00                      ; body6  token -- ECHOED
@80   81                         ; body8  0x01 | 0x80
@81   12                         ; body9  opcode -- ECHOED
@84   09 00 00 00                ; body12:16 epoch -- ECHOED (unmutated copy)
@88   10                         ; body16 role -- ECHOED (unmutated)
@89   04                         ; body17 OVERWRITTEN with own_class (0x04)
@90   01                         ; body18 response marker, FORCED
@92   09 00 00 00                ; body20:24 FRESH LE u32 copy of the epoch
