%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-valblk-convert
class:     scs-msg
origin:    spec-composed
spec:      tests/lab/captures/vms-c03-dlm-opcodes-20260911/GROUNDING.md (op 0x06 = CONVERT carrying the LVB, frame f14)
capture:   dlm-lvb3-20260911.pcap
wire-len:  204
sha256:    4707333fa8f9310fef3d7776cb5d9722f68cc7c59a530ef554499433104b8520
%bytes
; abs 0-71: plausible classifying prefix only -- see dlm-deq-release.spec.
@0    08 00 2b 78 56 b9          ; eth dst: VAX2 HW MAC (the master)
@6    08 00 2b 4a b7 15          ; eth src: VAX1 HW MAC (the writing node)
@12   60 07                      ; ethertype 0x6007
@14   bc 00                      ; SCA length field -> 190-byte content
@16   aa 00 04 00 02 04          ; dst logical: VAX2 (sysid 1026)
@22   01 00                      ; connect flag
@24   aa 00 04 00 01 04          ; src logical: VAX1 (sysid 1025)
@30   4b 13                      ; msgtype 0x4b (sequenced msg), format 0x13
;
; abs 72-: the DLM SYSAP body -- CITED.
;
; PROVENANCE. dlm-lvb3-20260911.pcap frame f14, VAX1 -> vax2. The driver took
; an EX lock on 'OVMXLVB3' with LCK$M_VALBLK (f12, op 0x01; granted at f13),
; wrote a known 16-byte pattern into LKSB+8, and converted EX -> NL with
; LCK$M_VALBLK. f14 below is the frame that crossing produced.
;
; WHY A KNOWN PATTERN IS THE WHOLE PROOF: the 16 bytes the driver wrote appear
; VERBATIM on the wire at a single position. There is no inference step
; between "the executive held this value block" and "these bytes are the value
; block" -- they are the same bytes.
@80   02                         ; body[8]  category 0x02 (request)
@81   06                         ; body[9]  opcode 0x06 -- the CONVERT that
                                 ; carries the value block; distinct from the
                                 ; op-0x07 CONVERT, which does not and which
                                 ; is the one that draws a cat-0x82 reply
@92   cd 01 00 27                ; body[20:24] req_lkid 0x270001cd == the
                                 ; requester handle f13's GRANT assigned
@96   89 04 00 2b                ; body[24:28] master_lkid 0x2b000489 == the
                                 ; master handle f12's ENQ for 'OVMXLVB3'
                                 ; carried
@102  00                         ; body[30] mode NL(0) -- converted DOWN from
                                 ; EX, which is the crossing that writes the
                                 ; block back to the master
@108  57 52 4f 54 45 42 59 56    ; body[36:52] THE 16-BYTE LOCK VALUE BLOCK:
@116  41 58 31 58 58 58 58 58    ; 'WROTEBYVAX1XXXXX', byte for byte the
                                 ; pattern the driver placed at LKSB+8
;
; DELIBERATELY NOT CITED: body[32:36], the four bytes immediately ahead of the
; block. They read 1f 00 01 00 here and 1e 00 01 00 on the other op-0x06
; frames in the capture set -- the low byte tracks a per-request index, which
; no capture pins. That is exactly why vms_cluster_codec_dlm.c has an op-0x06
; ACCESSOR and no op-0x06 BUILDER: composing a whole frame would mean minting
; these four bytes, and INV-6 has nothing to mint them from.
