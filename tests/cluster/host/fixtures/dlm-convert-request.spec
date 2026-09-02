%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-convert-request
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(f).1 (ac4-CVT)
capture:   scs-dlm-lockconflict.pcap
wire-len:  204
sha256:    0ed0e3b5e44074dab050accee2ab14ef3db347f953a6e8a4d296ab50e0303791
%bytes
; abs 0-71: not cited, see dlm-enq-request-pw.spec's header comment.
@0    08 00 2b 78 56 b9
@6    08 00 2b 4a b7 15
@12   60 07
@14   bc 00
@16   aa 00 04 00 02 04
@22   01 00
@24   aa 00 04 00 01 04
@30   4b 13
; abs 72-: the DLM body (cited). CONVERT (op 0x07): unlike a fresh ENQ, the
; lock already exists, so body[20] already carries the real local lock-id
; (not a PID placeholder) and body[24] the real master lock-id -- both
; GROUNDED byte-exact vs SDA (ac4-CVT). body[30] carries the NEW mode
; requested by the convert (GROUNDED: "NL->EX reads 05").
@80   02                         ; body[8]  category 0x02 (request)
@81   07                         ; body[9]  opcode 0x07 (CONVERT)
@92   8a 03 00 50                ; body[20:24] existing local lock-id
                                 ; 0x5000038A (GROUNDED vs SDA, ac4-CVT)
@96   b9 04 00 12                ; body[24:28] master lock-id 0x120004B9
                                 ; (GROUNDED vs SDA, ac4-CVT)
@102  05                         ; body[30] new mode EX=5 (GROUNDED,
                                 ; NL->EX one-variable diff, ac4-CVT)
; The resource name for the ac4-CVT specimen is not independently recorded
; in the published spec text (it names F11B$aSYSDSK1-family locks "seen
; incidentally" in this capture, not tied to THIS specific frame) -- left
; UNCITED rather than guessed.
