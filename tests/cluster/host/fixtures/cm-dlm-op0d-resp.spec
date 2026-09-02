%OVMX-CLUSTER-SPECIMEN-1
name:      cm-dlm-op0d-resp
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(p) "reconstructs 1367 of 1367 real responses byte-for-byte"
capture:   scs-dlm-lockconflict.pcap
wire-len:  204
sha256:    7d371cd96dd6ff197850f142ed2816552398d6928de4e81800f0a1139c2b4a5e
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
; The response to cm-dlm-op0d-req.spec: VERBATIM echo of the 132-byte
; body plus exactly send/ack + body[8]|=0x80 + body[34]=0xf9 (MANDATORY,
; unconditional -- overrides the request's 0x00). Deliberately does NOT
; take the cat-0x01 body[18]/body[55] mutations (there is nothing cited
; there in this specimen, which is itself part of the proof: applying
; them would corrupt the resource name at body[48], sec 4(p)'s
; LOCKMGRERR warning).
;
@72   08 00                      ; body0  OUR OWN send-msg#
@74   04 00                      ; body2  OUR OWN ack-msg#
@76   15 00                      ; body4  txn -- ECHOED
@78   99 00                      ; body6  token -- ECHOED
@80   82                         ; body8  0x02 | 0x80
@81   0d                         ; body9  opcode -- ECHOED
@84   01 00                      ; body12:14 -- ECHOED
@86   03 00                      ; body14:16 -- ECHOED
@88   10                         ; body16 L1 length -- ECHOED (NOT the cat-0x01
                                 ; body[18] mutation; this offset is untouched)
@106  f9                         ; body34 result stamp FORCED to 0xf9
@119  0d                         ; body47 resource-name length -- ECHOED
@120  46 31 31 42 24 61 53 59 53 44 53 4b 31
                                 ; body48.. "F11B$aSYSDSK1" -- ECHOED verbatim,
                                 ; byte-exact, uncorrupted
