%OVMX-CLUSTER-SPECIMEN-1
name:      cm-relay-oracle-req
class:     scs-msg
origin:    capture
capture:   vax3-2to3-established-join-20260730.pcap
spec:      docs/cluster-protocol-spec.md 4(p) "which relays the new node to the rest (op 0x12)", 4(O.31) "the op 0x12 RELAY sits between op 0x02 and op 0x03 and is the commit gate"
wire-len:  204
sha256:    1e6c9924e307f3d0fd7fba01f36d354a34bcc9c94ce567c75112c40898990249
%bytes
; REAL captured cat-0x01 op-0x12 RELAY REQUEST from a real OpenVMS VAX
; TRANSITION COORDINATOR (08:00:2b:78:56:b9) to a real sitting VMS MEMBER
; (aa:00:04:00:01:04, logical aa-00-04-00-01-04) while a THIRD node was
; being admitted to an established two-node cluster -- the Rule of Total
; Connectivity (VAXcluster Principles p. 7-39).
;
; This is the frame OVMX used to log as "an unroutable VMS$VAXcluster frame
; was received" and answer with nothing (rd vms-4f0). Its answer, which the
; coordinator gates the whole admission on, is cm-relay-oracle-resp.spec --
; the SAME real VAX's reply, 0.3 ms later, in the same capture.
;
; GROUNDED anchors: body[8:10] = 01 12 (cat/op); body[12:16] = epoch 3 (LE u32);
; body[16] = 0x10 ROLE_RELAY (spec 4(r) role census); body[17] = 0x02 the
; RELAYER's own class. body[20:24] holds 12 02 40 20 in the REQUEST -- the
; response OVERWRITES it with a fresh LE u32 copy of the epoch, which is the
; single mutation that cannot come from an echo.
;
; Full frame CITED verbatim (real capture), including the volume-name block at
; body[56:74] ("F11B$aSYSDSK1") and the per-frame trailer -- preserved as
; captured, never named or fabricated.
@0    aa 00 04 00 01 04 08 00 2b 78 56 b9 60 07 bc 00
@16   aa 00 04 00 01 04 01 00 aa 00 04 00 02 04 4b 13
@32   e1 48 32 5d 01 00 12 00 e1 48 00 00 32 5d 00 00
@48   e1 48 00 00 01 00 00 02 92 00 04 00 0a 00 02 00
@64   0a 00 51 35 09 00 98 a4 6c 52 19 3e 01 00 a8 a0
@80   01 12 00 00 03 00 00 00 10 02 00 00 12 02 40 20
@96   a9 03 00 03 64 00 00 00 00 00 f9 60 12 aa 87 00
@112  00 00 00 00 00 00 00 16 46 31 31 42 24 61 53 59
@128  53 44 53 4b 31 20 20 20 20 20 2a 00 00 00 00 00
@144  01 00 01 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  01 00 00 00 01 00 00 00 00 00 00 00 00 00 00 00
@176  de 00 00 00 00 00 01 01 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff 6d 1b 50 48
