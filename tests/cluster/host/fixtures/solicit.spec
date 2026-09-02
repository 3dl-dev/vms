%OVMX-CLUSTER-SPECIMEN-1
name:      solicit-vax3-satellite-boot
class:     solicit
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 2, 3, 4(a), 4(c)
capture:   satellite-niscs-boot-solicit.pcap
frame:     1100
wire-len:  92
sha256:    62c224cc7c16e5c5272068e178bc45aabb1c4ae55da89461020b41db0b8ae8f7
%bytes
@0    ab 00 04 01 01 01          ; eth dst = cluster multicast group 1
@6    aa 00 04 00 03 04          ; eth src: sec 2 allows the LOGICAL LAVC MAC
                                 ; here; VAX3's is from sec 3. The satellite's
                                 ; raw HW MAC is not published for this capture.
@12   60 07
@14   4c 00                      ; 0x004c + 2 = 78-byte SCA content (sec 2)
@16   ab 00 04 01 01 01          ; dst/group logical addr = the group
@22   01 00
@24   aa 00 04 00 03 04          ; src logical addr, VAX3 (sec 3)
@30   b6 00                      ; per-frame word b6 on a VAX3 SOLICIT (sec 4a)
@32   08 00 00 80
@36   02                         ; message-class byte 0x02 = SOLICIT (sec 4a)
@37   01 00 00
@40   06
@41   56 41 58 33 20 20          ; "VAX3  "
@68   ee 05 39 5b                ; join nonce, identical on the SOLICIT (GROUNDED sec 4a)
@72   00 00 00 00                ; sec 4c: 4 zero bytes
@76   09                         ; target device spec length (GROUNDED sec 4c)
@77   5f 24 32 24 44 55 41 30 3a ; "_$2$DUA0:" -- "serve me this system disk"
@86   00 00 00 00 00 00          ; trailing zero pad (sec 4c)
