%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-grant-valblk
class:     scs-msg
origin:    capture
capture:   c2-seq.pcap
wire-len:  204
sha256:    1705b37b06bc3f738f17bcf2432d2cfcee2eaa4170fac0957ec68aef586e0307
%bytes
; abs 0-71: classifying prefix only (eth + SCA/LAVC header) -- cited so the
; frame classifies, not compared against the codec's body-only build.
@0    aa 00 04 00 01 04         ; eth dst: VAX1 cluster-logical (the requester)
@6    08 00 2b a9 a3 96         ; eth src: the master's HW MAC
@12   60 07                     ; ethertype 0x6007
@14   bc 00                     ; SCA length field -> 190-byte content
@16   aa 00 04 00 01 04         ; dst logical: VAX1 (the requester)
@22   01 00                     ; connect flag
@24   aa 00 04 00 02 04         ; src logical: VAX2 (the master)
@30   4b 13                     ; msgtype 0x4b (sequenced msg), format 0x13
;
; abs 72-: the DLM SYSAP body -- CITED.
;
; PROVENANCE. c2-seq.pcap, the grant reply VAX2(master) -> VAX1. VAX1's DLMLVB3
; driver took an EX lock on 'OVMXLV01' with LCK$M_VALBLK and wrote the 16-byte
; pattern below; a fresh $ENQW then re-requested the lock and the MASTER returned
; the resource's current value block IN THE GRANT -- the LVB READ crossing. The
; 16 bytes appear VERBATIM at body[36:52]: there is no inference between "the
; master held this value block" and "these bytes are the value block". SDA
; SHOW LOCKS on VAX2 lists 'Lock id: 0A0003A4' on 'OVMXLV01', flags VALBLK, which
; matches body[20:24] byte-for-byte.
;
@80   82                        ; body[8]  category 0x82 (RESPONSE)
@81   01                        ; body[9]  opcode 0x01 -- an ENQ GRANT reply
@84   01 00 02 00               ; body[12:16] HDR words 0x0001,0x0002 -- the same
                                ; constant the op-0x06 write carries (per-op const)
@92   a4 03 00 0a               ; body[20:24] req_lkid 0x0a0003a4 -- the requester's
                                ; own handle (== SDA Lock id 0A0003A4)
@96   b7 01 00 57               ; body[24:28] master_lkid 0x570001b7 -- the master
                                ; handle the ENQ for 'OVMXLV01' carried
@100  10                        ; body[28] the GRANT-WITH-VALBLK record flag 0x10.
                                ; CONSTANT across nine+ grant-with-valblk frames
                                ; (c1/c2/c4/c5/c6/c7 + vms-c03 blk2/deq/lvb3);
                                ; contrast the op-0x06 write's 0x13 and a plain
                                ; ENQ request's 0x11.
@102  00                        ; body[30] granted mode NL(0)
@104  01 00 fa 00               ; body[32:36] the grant-valblk record word. body[32]
                                ; =0x01 (CONSTANT across all nine+ -- NOT the write's
                                ; per-lock SERIAL), body[34]=0xfa the cat-0x82 REPLY
                                ; stamp (a REQUEST carries 0x01 here, a REPLY 0xfa/
                                ; 0xf9; grounded in the op-0x06 spec).
@108  57 52 4f 54 45 42 59 56   ; body[36:52] THE 16-BYTE LOCK VALUE BLOCK returned
@116  41 58 31 58 58 58 58 58   ; by the master: 'WROTEBYVAX1XXXXX', byte for byte
                                ; the pattern the requester earlier wrote at LKSB+8.
;
; body[52:88] is a sequence-like word at body[52:54] (an SCS-layer counter that
; varies across captures and this DLM codec does not own) followed by
; uninitialised sender buffer -- NEITHER cited NOR reproduced. The builder
; zero-fills the span, exactly as the op-0x06 builder zero-fills its own tail.
