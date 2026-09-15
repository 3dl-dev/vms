%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-valblk-convert
class:     scs-msg
origin:    spec-composed
spec:      tests/lab/captures/vms-c03-dlm-opcodes-20260911/GROUNDING.md (op 0x06 = CONVERT carrying the LVB, frame f14)
capture:   dlm-lvb3-20260911.pcap
wire-len:  204
sha256:    3741fcad5241c8c259298b2aac064edc4b55f8c386bb210755649ef9674da405
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
@84   01 00 02 00                ; body[12:16] op-0x06 header words 0x0001,
                                 ; 0x0002 -- constant in every op-0x06 frame
                                 ; (grounded vms-727; the rebuild op carries
                                 ; 0x0001,0x0003 here, so this is per-op)
@92   cd 01 00 27                ; body[20:24] req_lkid 0x270001cd == the
                                 ; requester handle f13's GRANT assigned
@96   89 04 00 2b                ; body[24:28] master_lkid 0x2b000489 == the
                                 ; master handle f12's ENQ for 'OVMXLVB3'
                                 ; carried
@100  13                         ; body[28] op-0x06 request flag 0x13 -- an ENQ
                                 ; request carries 0x11 here (grounded vms-727)
@102  00                         ; body[30] mode NL(0) -- converted DOWN from
                                 ; EX, which is the crossing that writes the
                                 ; block back to the master
@104  1f                         ; body[32] per-lock SERIAL. INFERRED (vms-727):
                                 ; the low byte of the lock's ENQ request id,
                                 ; carried through every frame for this lock and
                                 ; re-stamped at body[52]. Constant across three
                                 ; writes to one held lock; advances only across
                                 ; distinct locks -- NOT a valblk sequence.
@106  01                         ; body[34] cat-0x02 REQUEST stamp 0x01 (a REPLY
                                 ; carries 0xfa/0xf9 here; grounded vms-727)
@108  57 52 4f 54 45 42 59 56    ; body[36:52] THE 16-BYTE LOCK VALUE BLOCK:
@116  41 58 31 58 58 58 58 58    ; 'WROTEBYVAX1XXXXX', byte for byte the
                                 ; pattern the driver placed at LKSB+8
@124  1f 02 20 20                ; body[52:56] the closing bracket: SERIAL
                                 ; (== body[32]) then 0x02,0x20,0x20 -- constant
                                 ; across all five vms-727 captures, so a stable
                                 ; field, not stale buffer.
;
; body[32:36] and body[52:56] were UN-CITED before vms-727 ("the low byte
; tracks a per-request index, which no capture pins"). The vms-727 own-lab
; campaign (5 real-wire one-variable-diff captures, byte-verified) pinned them:
; body[34]=0x01 is the request stamp, body[32]==body[52] is a per-LOCK serial
; sourced from the LKB, and the surrounding bytes are op-0x06 constants. So the
; op-0x06 BUILDER now exists (vms_dlm_valblk_convert_build) -- every byte is
; sourced from the LKB or grounded, none minted (INV-6). body[56:88] stays
; UN-CITED: it is uninitialised sender buffer on the real wire (VAX P1 stack
; addresses, inconsistent between captures), which the builder zero-fills
; rather than reproduce.
