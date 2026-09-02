%OVMX-CLUSTER-SPECIMEN-1
name:      scs-start-ack-round2
class:     scs-start
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 2, 3, 4(g) phase 2
capture:   formation-ci1-joinwindow.pcap
frame:     20
wire-len:  60
sha256:    ebec8a6b91a001798bb84bbf9a245755a1603ed7c63d7ba84f2992e934f03980
%bytes
;
; The 46-byte-payload ACK of the START/STACK/ACK round (sec 4g phase 2; the
; naming correction from VAXcluster Principles p.2-12). Payload 46 + the
; 14-byte Ethernet header is exactly 60, so this frame is FULLY cited and its
; sec 2 length identity resolves EXACT, not runt-padded -- the other half of
; the runt rule the credit-short specimen exercises.
;
@0    08 00 2b 78 56 b9          ; eth dst = VAX2 HW MAC
@6    08 00 2b 4a b7 15          ; eth src = VAX1 HW MAC
@12   60 07
@14   2c 00                      ; 0x002c + 2 = 46-byte payload
@16   aa 00 04 00 02 04          ; pl2  dst logical (VAX2)
@22   01 00                      ; pl8
@24   aa 00 04 00 01 04          ; pl10 src logical (VAX1)
@30   41                         ; pl16 msgtype 0x41
@31   13                         ; pl17 format 0x13
@32   00 00                      ; pl18
@34   03 00                      ; pl20 send_seq
@36   01 00                      ; pl22 counter B / incarnation echo
@38   12 00                      ; pl24 NISCS_LAN_OVRHD 18
@40   00 00 00 00                ; pl26
@44   03 00                      ; pl30 mirror of pl20
@46   00 00 00 00 00 00          ; pl32
@52   01 00                      ; pl38
@54   00 00                      ; pl40
@56   02 00                      ; pl42 inner length = 46 - 44 = 2 (GROUNDED 42/42)
@58   02 00                      ; pl44 config round 2 = the ACK (GROUNDED)
