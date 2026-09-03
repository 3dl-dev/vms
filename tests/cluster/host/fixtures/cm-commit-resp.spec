%OVMX-CLUSTER-SPECIMEN-1
name:      cm-commit-resp
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(p) "The 0x81 echo takes THREE mutations"
capture:   formation-ci1-joinwindow.pcap
wire-len:  204
sha256:    21f92a324dc3db2cbe7fe0a2e42d8bd391cc7f080f890d3d6b1a813e3035ae62
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
; The GROUNDED 0x81 response to cm-commit-req.spec. op 0x03 is NOT op
; 0x09, so body[55] is left ALONE (echoed, not cleared) -- there is
; nothing at body[55] in this specimen to echo (residue zero), which is
; itself part of the proof: only op 0x09 gets that mutation.
;
@72   30 00                      ; body0  OUR OWN send-msg#
@74   04 00                      ; body2  OUR OWN ack-msg#
@76   11 00                      ; body4  txn -- ECHOED
@78   22 cc                      ; body6  token -- ECHOED
@80   81                         ; body8  0x01 | 0x80
@81   03                         ; body9  opcode -- ECHOED
@88   20                         ; body16 role -- ECHOED (unmutated)
@89   02                         ; body17 class -- ECHOED (unmutated)
@90   01                         ; body18 response marker, FORCED
@172  77                         ; body100 residue byte -- ECHOED verbatim
