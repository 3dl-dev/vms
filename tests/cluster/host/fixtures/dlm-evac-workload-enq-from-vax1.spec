%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-evac-workload-enq-from-vax1
class:     scs-msg
origin:    capture
capture:   ev2-mixed-refusal.pcap
spec:      docs/cluster-protocol-spec.md 4(f) + 4(j) (rd vms-025, the 2026-10-09 lab finding)
frame:     3907
wire-len:  204
sha256:    55c73013806705a15309c6fdec6059454b627b13c4728a5ee6766597750ae39f
%bytes
; THE FRAME THE MIXED-CLUSTER ARM REFUSED, captured verbatim.
;
; tests/lab/captures/vms-025-mixed-dlm-refusal-20261009/ev2-mixed-refusal.pcap,
; record 3907 (0-based) -- a real OpenVMS VAX V7.3 (VAX1, SCSSYSTEMID 1025) on a
; mixed 3-member cluster (group 1) sending its ONE cat-0x02 op-0x01 ENQ for the
; user-mode resource EVAC$WORKLOAD to the OVMX member OVMXE (1030, HW MAC
; 52:54:00:00:e5:01), which already held an NL lock on that name AND mastered it.
;
; OVMXE answered NOTHING and logged
;   %DLM, a VMS system asked this node, its lock directory, for a resource this
;         node itself holds locks on and does not master: not answered
;   %DLM, refusing a lock message from a system that has not proved it runs this
;         implementation
; and VAX1's $ENQW(EX) hung forever. EX is compatible with the NL this node held,
; so the faithful answer was a GRANT (Davis p. 6-31 outcome (a), p. 6-51).
;
; The cause was NOT in this frame: vms_ldwv_sole_directory() was FALSE on OVMXE,
; because `SYSBOOT> SET LOCKDIRWT 1` had been answered %SYSGEN-E-NOSUCHP (the
; shipped OVMXVMSSYS.PAR carried no LOCKDIRWT row), so OVMXE joined at weight 0,
; the all-zero rule gave one vector entry per system, and every arm of the
; mixed-cluster DLM was gated off. This specimen is the INPUT that reproduces
; that refusal on the host, and the same input the engine grants once the
; configuration really holds.
;
; --- envelope, spec 4(d)/4(j) --------------------------------------------
@0    52 54 00 00 e5 01            ; eth dst: OVMXE's HW MAC (the OVMX member)
@6    aa 00 04 00 01 04            ; eth src: VAX1's cluster-logical LAVC addr
@12   60 07                        ; ethertype 0x6007 (SCA)
@14   bc 00                        ; SCA length 188 -> the 204-byte frame class
@16   aa 00 04 00 06 04            ; dst logical
@22   01 00                        ; connect flag
@24   aa 00 04 00 01 04            ; src logical (VAX1)
@30   4b 13                        ; span this codec does not own
@32   66 00 77 00 01 00 12 00 66 00 00 00 77 00 00 00
@48   66 00 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   09 00 f6 6a 0d 00 ad 1c
;
; --- the DLM SYSAP body (body[0] = abs 72) -------------------------------
;   abs 80  body[8]   category 0x02  = a cat-0x02 DLM REQUEST
;   abs 81  body[9]   opcode   0x01  = ENQ, a new lock request
;   abs 92  body[20]  0x2020021e     = VAX1's requesting PID (EVACVAX1)
;   abs 96  body[24]  0x1a00021d     = the requester's own lock handle
;   abs 102 body[30]  0x05           = EX, the mode requested
;   abs 116 body[44]  0x0001         = the resource's UIC GROUP (group 1)
;   abs 118 body[46]  0x03           = the resource's ACCESS MODE (user)
;   abs 119 body[47]  0x0d           = resource-name length, 13
;   abs 120 body[48]  "EVAC$WORKLOAD"
;   abs 200 body[128] 0x00027e10     = the DIRECTORY HASH VAX1 ITSELF put on the
;                                      wire for this name (Davis p. 6-50) -- the
;                                      only value OVMX may ever assert for it
@72   3b 00 2a 00                  ; body[0:2] send-msg, body[2:4] ack-msg
@76   08 00 e5 0d                  ; body[4:6] txn, body[6:8] checksum
@80   02 01                        ; category 0x02, opcode 0x01 (ENQ)
@82   41 50 01 00 02 00 00 00 00 00
@92   1e 02 20 20                  ; body[20:24] requesting PID 0x2020021e
@96   1d 02 00 1a                  ; body[24:28] requester lock handle 0x1a00021d
@100  00 00                        ; body[28:30]
@102  05 00                        ; body[30] mode 0x05 = EX
@104  1e 00 01 00 00 00 00 00 00 00 00 00
@116  01 00                        ; body[44:46] UIC group 1
@118  03                           ; body[46] access mode 3 = user
@119  0d                           ; body[47] resource-name length 13
@120  45 56 41 43 24 57 4f 52 4b 4c 4f 41 44   ; "EVAC$WORKLOAD"
@133  41 20 20 20 20 04 00 00 00 00 00
@144  50 57 f8 7f 78 00 00 00 00 00 00 00 71 00 00 00
@160  84 48 f8 7f 00 00 00 00 00 41 00 00 00 00 2e 01
@176  01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 ff ff ff ff
@200  10 7e 02 00                  ; body[128:132] the VAX's OWN directory hash
