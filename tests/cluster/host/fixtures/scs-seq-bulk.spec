%OVMX-CLUSTER-SPECIMEN-1
name:      scs-seq-bulk-block-transfer
class:     scs-seq
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 2 Table 2, 3, 4(d), 4(e)
capture:   formation-ci1.pcap
frame:     6100
wire-len:  316
sha256:    0687185659f842a66a2d7f5332f9623db19cb8e3b0a59a983432b3a36b948103
%bytes
;
; A 0x4b/0x13 sequenced-application frame from the MSCP bulk block-transfer
; class (sec 2 Table 2 lists content lengths 526/398/634/462/270/82/369/302/
; 590/718; sec 4e records the header as NOT decoded).
;
; This is the specimen for the deliberately-modest class VMS_FCLS_SCS_SEQ:
; the shared SCA envelope is grounded, and NOTHING else is. In particular the
; class carries no CONID capability, because sec 4(d) says of every non-190
; length class that it does "not reliably match this layout and [is] therefore
; left undecoded" -- so vms_scs_conid() must REFUSE this frame rather than hand
; back the four bytes that happen to sit at abs 64.
;
@0    08 00 2b 4a b7 15          ; eth dst = VAX1 HW MAC
@6    08 00 2b 78 56 b9          ; eth src = VAX2 HW MAC
@12   60 07
@14   2c 01                      ; 0x012c + 2 = 302-byte payload
@16   aa 00 04 00 01 04          ; pl2  dst logical (VAX1)
@22   01 00                      ; pl8
@24   aa 00 04 00 02 04          ; pl10 src logical (VAX2)
@30   4b                         ; pl16 msgtype 0x4b
@31   13                         ; pl17 format 0x13
