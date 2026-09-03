%OVMX-CLUSTER-SPECIMEN-1
name:      scs-dir-connect-request
class:     scs-conn-ctrl
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(h)(1), 4(h)(2), 4(h)(2a), 4(m) op=0 CONNECT-REQUEST
capture:   formation-ci1-joinwindow.pcap
frame:     21
wire-len:  124
sha256:    9235a521ea69d6c370211124716ff122baf3d9e96ca808c0e017f2146e9d85fa
%bytes
;
; The SCS$DIRECTORY op-0 CONNECT_REQ (sec 4(h)(2)/(2a)): VAX1's Process
; Poller opening its own directory connection to VAX2. 110-byte content,
; byte-exact, harvested from src/vmsscs/scs_dir.c dir_connreq_tmpl
; (raw pcap frame 29, SCA 21, formation-ci1-joinwindow.pcap).
;
; This is the SAME wire shape as scs-conn-ctrl.spec's VMS$VAXcluster
; CONNECT_REQ (op=0, 110 bytes) but a DIFFERENT specimen -- the
; SCS$DIRECTORY dialogue -- chosen because it is grounded byte-exact
; through the FULL 110 bytes, including the [62:94] SYSAP name pair.
;
@0    08 00 2b 78 56 b9           ; eth dst = VAX2 HW MAC
@6    08 00 2b 4a b7 15           ; eth src = VAX1 HW MAC
@12   60 07                       ; ethertype 0x6007
@14   6c 00                       ; pl0  length = 0x006c (110-byte content)
@16   aa 00 04 00 02 04           ; pl2  dst logical (VAX2)
@22   01 00                       ; pl8  connect flag
@24   aa 00 04 00 01 04           ; pl10 src logical (VAX1)
@30   5b 13                       ; pl16 msgtype 0x5b / format 0x13
@32   00 00 01 00 01 00 12 00     ; pl18..25 recv_ack/send_seq/incarn/NISCS_LAN_OVRHD
@40   00 00 00 00 01 00 00 00 00 00 00 00 01 00 00 02 42 00 ; pl26..43 counter mirrors + inner length=0x0042(66)
@58   04 00                       ; pl44 format word 0x0004 (sec 4h(1b))
@60   00 00                       ; pl46 op = 0 CONNECT_REQ
@62   03 00                       ; pl48 credit = 3 (SCS$DIRECTORY Send Credits, sec 4d)
@64   00 00 00 00                 ; pl50 remote Con.ID = 0 (target CDT not yet formed)
@68   08 00 05 63                 ; pl54 local Con.ID (offered)
@72   00 00 01 00                 ; pl58 marker 00 00 01 00
@76   53 43 53 24 44 49 52 45 43 54 4f 52 59 20 20 20 ; pl62 target SYSAP name 'SCS$DIRECTORY   '
@92   53 43 53 24 44 49 52 5f 4c 4f 4f 4b 55 50 20 20 ; pl78 source SYSAP name 'SCS$DIR_LOOKUP  ' -- the span
;      scs-conn-ctrl.spec left uncited (this is the FC-P2.1 harvest)
@108  20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 ; pl94 trailing blanks
