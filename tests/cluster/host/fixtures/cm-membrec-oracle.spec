%OVMX-CLUSTER-SPECIMEN-1
name:      cm-membrec-oracle
class:     scs-msg
origin:    capture
capture:   vax3-2to3-established-join-20260730.pcap
spec:      docs/cluster-protocol-spec.md 4(o) rows 8-9 + 4(r); rd vms-1ac "the op-0x05 record carries its transition's epoch"
wire-len:  204
sha256:    5648a4388fae330daad66ee44918b85bfd9ec4870ecff93b7c56846372f133ae
%bytes
; REAL captured cat-0x01 op-0x05 MEMBERSHIP RECORD, emitted by a real OpenVMS
; VAX acting as TRANSITION COORDINATOR while admitting a third node -- and,
; decisively for rd vms-1ac, a NON-FOUNDER member doing it: VAX2 (SCSSYSTEMID
; 1026) admitting VAX3 (1027) into the established cluster {VAX1, VAX2}.
; This record is the one it sends the JOINER about VAX1.
;
; THE FIELD THIS SPECIMEN EXISTS FOR is body[12:16] = 04 00 00 00 -- the EPOCH
; of the transition the record belongs to. OVMX left it ZERO on every record it
; ever sent, and a real OpenVMS VAX V7.3 handed membership records for a
; transition it could not place took a fatal CNXMGRERR bugcheck (rd vms-1ac,
; tests/lab/captures/vms-4f0-cn3-relay-20260924/). Corpus-wide, all 791 real
; op-0x05 records carry their transition's epoch, and it is always the
; coordinator's op-0x12 RELAY epoch plus one (133/133 relay->commit pairs).
;
; The other grounded fields, for the record:
;   body[16:20] = 20 02 00 00   role 0x20 / class 0x02 (VMS_CM_MEMBREC_TAG)
;   body[20:24] = 01 04 00 00   SCSSYSTEMID 1025, the system described
;   body[28:36] = f9 b8 2d 7c 9a 00 bc 00   its boot time (VMS quadword)
;   body[36:40] = 01 00 01 00   the CSID the cluster assigned it, 0x00010001
;   body[40:42] = 00 00         its 0-based CSV index
; and body[42:132] is zero on this specimen (sec 5c: uninterpreted).
@0    08 00 2b 11 22 33 08 00 2b 78 56 b9 60 07 bc 00
@16   aa 00 04 00 03 04 01 00 aa 00 04 00 02 04 4b 13
@32   21 00 22 00 01 00 12 00 21 00 00 00 22 00 00 00
@48   21 00 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   0a 00 e3 18 0d 00 98 a4 05 00 04 00 09 00 a5 93
@80   01 05 00 00 04 00 00 00 20 02 00 00 01 04 00 00
@96   00 00 00 00 f9 b8 2d 7c 9a 00 bc 00 01 00 01 00
@112  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@128  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 00 00 00 00
