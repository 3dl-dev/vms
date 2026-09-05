%OVMX-CLUSTER-SPECIMEN-1
name:      scs-dir-connect-confirm
class:     scs-conn-ctrl
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(g) phase 4, 4(h)(1), 4(m) op=3 CONNECT-CONFIRM
capture:   formation-clean-2node.pcap
frame:     26
wire-len:  76
sha256:    a95d92e0867a5feba29203f00c316a92a904e9b4fd5c254694a6c12166fcc9d4
%bytes
;
; The op-3 CONNECT-CONFIRM (sec 4(g) phase 4, 4(m)): the load-bearing
; confirm that binds a connection for use (sec 4(m) 'the VC confirm
; gates the entire membership dialogue'). 62-byte content, byte-exact,
; harvested from src/vmsscs/scs_dir.c dir_confirm_tmpl (formation-
; clean-2node.pcap SCA idx 26).
;
; The Ethernet dst/src HW MAC for THIS capture is INFERRED, not
; independently re-verified: the payload's own logical addresses
; (aa:00:04:00:01:04 / :02:04) name SCSSYSTEMID 1/2 -- the SAME VAX1/
; VAX2 lab pair whose real HW MACs (08:00:2b:4a:b7:15 /
; 08:00:2b:78:56:b9) are grounded across every OTHER capture in this
; corpus. Cited on that identity match, not fabricated from nothing.
; The connect-flag and sequence-counter region ARE known (the strawman
; carries them byte-exact) and are cited below.
;
@0    08 00 2b 4a b7 15           ; eth dst = VAX1 HW MAC (inferred, see above)
@6    08 00 2b 78 56 b9           ; eth src = VAX2 HW MAC (inferred, see above)
@12   60 07                       ; ethertype 0x6007
@14   3c 00                       ; pl0  length = 0x003c (62-byte content)
@16   aa 00 04 00 01 04           ; pl2  dst logical (VAX1)
@22   01 00                       ; pl8  connect flag
@24   aa 00 04 00 02 04           ; pl10 src logical (VAX2)
@30   5b 13                       ; pl16 msgtype 0x5b / format 0x13
@32   02 00 02 00 01 00 12 00     ; pl18..25 recv_ack/send_seq/incarn/NISCS_LAN_OVRHD
@40   02 00 00 00 02 00 00 00 02 00 00 00 01 00 00 02 12 00 ; pl26..43 counter mirrors + inner length=0x0012(18)
@58   04 00                       ; pl44 format word 0x0004
@60   03 00                       ; pl46 op = 3 CONNECT-CONFIRM
@62   00 00                       ; pl48 credit = 0
@64   08 00 dc e2                 ; pl50 remote Con.ID (the member's)
@68   07 00 00 00                 ; pl54 local Con.ID (ours)
@72   00 00 01 00                 ; pl58 marker 00 00 01 00
