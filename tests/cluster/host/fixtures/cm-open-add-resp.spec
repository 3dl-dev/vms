%OVMX-CLUSTER-SPECIMEN-1
name:      cm-open-add-resp
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(p) "The 0x81 echo takes THREE mutations"
capture:   formation-ci1-joinwindow.pcap
wire-len:  204
sha256:    cc8885527d320e08c61a2c8fec6cb0d827c3c126a56f333c498d551cd2ab5076
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
; The GROUNDED 0x81 response to cm-open-add-req.spec: echo + THREE
; mutations -- body[8]|=0x80, body[18]=0x01, body[55]=0x00 (op 0x09 ONLY,
; spec 4(p): "the responder is refusing to assert" the coordinator's own
; bitmap). Only the BODY span (abs 72+, VMS_CM_BODY_LEN) is cited: the
; wrapping SCS envelope this file's vms_cm_link_build() produces is a
; separate, already-proven concern (sec 3 of vms_cluster_codec_cm.h).
;
@72   10 00                      ; body0  OUR OWN send-msg# (caller-supplied)
@74   06 00                      ; body2  OUR OWN ack-msg#
@76   07 00                      ; body4  txn -- ECHOED from the request
@78   ab 00                      ; body6  token -- ECHOED from the request
@80   81                         ; body8  0x01 | 0x80 response bit
@81   09                         ; body9  opcode -- ECHOED
@84   06 00 00 00                ; body12:16 epoch -- ECHOED (unmutated)
@88   40                         ; body16 role -- ECHOED (unmutated)
@89   02                         ; body17 class -- ECHOED (unmutated; op 0x12 only)
@90   01                         ; body18 response marker, FORCED (op != 0x0f)
@127  00                         ; body55 bitmap CLEARED (op 0x09 only)
