%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-real-enq-grant
class:     scs-msg
origin:    capture
spec:      docs/cluster-protocol-spec.md 4(f).1 (rd vms-b5b0)
capture:   vms-b5b0-storm-window.pcap
wire-len:  204
sha256:    887c7bfd969231e83fa2dc799a438b2a471fc4b39d2b820bbbb30f38c0c30f3a
%bytes
; THE REAL GRANT that answers dlm-real-enq-request: VAX2 -> VAX1, SCA frame 2
; of the same capture, 155 us later. One of 38 such grants in that capture, all of
; which agree on every field this specimen cites. EVERY byte is cited.
@0    aa 00 04 00 01 04 aa 00 04 00 02 04 60 07 bc 00
@16   aa 00 04 00 01 04 01 00 aa 00 04 00 02 04 4b 13
@32   25 59 fd 4a 01 00 12 00 25 59 00 00 fd 4a 00 00
@48   25 59 00 00 01 00 00 02 92 00 04 00 0a 00 02 00
@64   09 00 ad 1c 09 00 ad 1c 1a 43 42 51 04 00 8a 15
@80   82 01 00 00 01 00 07 00 00 00 00 00 b0 06 00 65
@96   cd 03 00 09 10 00 00 00 01 00 fa 00 01 00 00 00
@112  01 00 00 00 00 00 00 00 00 00 00 00 e4 0b 00 00
@128  00 00 49 4f 4e 5f 44 41 54 41 42 41 53 45 00 00
@144  02 00 01 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  00 00 00 03 00 00 00 00 04 00 ff ff ff ff ff ff
@176  e0 ff 00 60 ee 78 de 00 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff d7 68 a6 0b
;
; body[8]     = 82           the response bit
; body[20:24] = b0 06 00 65  THE MASTER's handle, ASSIGNED here (it appears
;                            nowhere in the request, and VAX1 carries it back
;                            at body[20:24] of its later op-03 release)
; body[24:28] = cd 03 00 09  THE REQUESTER's handle, ECHOED unchanged -- the
;                            correlation
; body[28]    = 10           the grant record flag (38/38)
; body[30]    = 00           the mode byte is CLEARED (38/38): a grant
;                            carries no granted mode
; body[32:36] = 01 00 fa 00  the grant record; body[34]=0xfa is THE OUTCOME
;                            byte (38/38), the same position a directory
;                            answer carries 0xf9/0xf8 in
; body[36:52] = 01 00 00 00  the MASTER RESOURCE'S VALUE BLOCK (16 bytes).
;               01 00 00 00  The span is grounded by the op-0x06 write
;               00 ...       crossing and by dlm-grant-valblk, a real grant
;                            that returns the requester's own 16 bytes. Note
;                            it COVERS body[44:52], where a REQUEST carries
;                            the identity and the first name bytes -- the two
;                            layouts overlap and never co-occur.
; body[52:54] = e4 0b        an SCS-layer word this DLM codec does not own
;                            (OVMX leaves it zero: the one span of this frame
;                            OVMX does not reproduce)
; body[54:58] = 00 00 00 00  the tail of the request's 10-byte name, cleared
; body[58:]   = ...          the requester's own STALE BUFFER ("ION_DATABASE"
;                            and the longwords after it), echoed VERBATIM --
;                            identical in the request and in this grant
