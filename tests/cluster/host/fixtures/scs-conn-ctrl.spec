%OVMX-CLUSTER-SPECIMEN-1
name:      scs-connect-request-vaxcluster
class:     scs-conn-ctrl
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 2, 3, 4(g) phase 4, 4(h)(1), 4(h)(1a), 4(h) erratum
capture:   formation-ci1-joinwindow.pcap
frame:     39
wire-len:  124
sha256:    7d173c0230515760c635fb7e06cc1d620021286a0ad90ad64bd8b8fe02c25431
%bytes
;
; The sec 4(g) phase-4 CONNECT-REQUEST that binds the VMS$VAXcluster VC: the
; keystone finding, GROUNDED against the SDA SHOW CONNECTIONS decoder ring.
; 110-byte payload -> 124 on the wire (the sec 4h erratum's content-vs-wire
; correction).
;
@0    08 00 2b 78 56 b9          ; eth dst = VAX2 HW MAC
@6    08 00 2b 4a b7 15          ; eth src = VAX1 HW MAC
@12   60 07
@14   6c 00                      ; 0x006c + 2 = 110-byte payload
@16   aa 00 04 00 02 04          ; pl2  dst logical (VAX2)
@22   01 00                      ; pl8
@24   aa 00 04 00 01 04          ; pl10 src logical (VAX1)
@30   4b                         ; pl16 msgtype 0x4b
@31   13                         ; pl17 format 0x13
@38   12 00                      ; pl24 NISCS_LAN_OVRHD 18 (the shared counter
                                 ;      region beginning at pl18, sec 4h)
@60   00 00                      ; pl46 SCA connection-control message type 0 =
                                 ;      CONNECT_REQ (GROUNDED sec 4h(1a),
                                 ;      60 frames / 16 dialogues / 0 residuals)
@64   00 00 00 00                ; pl50 remote Con.ID: still 0, the target's CDT
                                 ;      does not exist yet (GROUNDED sec 4g ph4)
@68   09 00 c5 62                ; pl54 local Con.ID 0x62C50009 = VAX1's own,
                                 ;      byte-exact to SDA "Local Con. ID"
@76   56 4d 53 24 56 41 58 63 6c 75 73 74 65 72 20 20
                                 ; pl62 local SYSAP name, 16-byte field:
                                 ; "VMS$VAXcluster  " (sec 4g ph4 + 4h erratum)
;
; pl78.. (abs 92..) carries the remote endpoint's SYSAP name and the rest of
; the connect body; uncited here, harvested by FC-P2.1.
