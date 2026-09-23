%OVMX-CLUSTER-SPECIMEN-1
name:      hello-c3-vaxc-v55-multicast
class:     hello-c3
origin:    capture
spec:      docs/cluster-protocol-spec.md 2, 4(a), 4(b), 4(b.c3)
capture:   hub-frames.json
frame:     0
wire-len:  128
sha256:    5dca677fa214ed2fcc8324b34a3083213be486ecd5e08fc8b930c389f5301e54
%bytes
;
; rd vms-0f8. A REAL OpenVMS VAX **V5.5-2H4** node's periodic multicast
; discovery advertisement, extracted VERBATIM from
; tests/lab/captures/vms147-browser-nodea-vaxc-20260922/hub-frames.json
; (the first of 53 identical-but-for-the-tick frames from `VAXC`; that file's
; SHA-256 is in docs/clean-room/reference-captures.sha256). The sender is an
; unmodified V5.5-2H4 system disk in the in-browser cluster demo, configured
; for cluster group 257. Nothing here is composed, inferred or reconstructed:
; every byte below is what that machine put on the wire.
;
; This is the SECOND discovery revision (vms_cluster_codec_hello.h's revision
; table). Against the sec 4(a)/4(b) revision every V7.3 node in the clean-room
; corpus speaks, it differs in exactly five field VALUES and one absent tail --
; and in nothing else. The rest of this file is the proof, field by field.
;
@0    ab 00 04 01 01 02          ; eth dst = cluster multicast, group 257 (sec 3, rd vms-147)
@6    52 54 00 00 00 0c          ; eth src = VAXC's real HW MAC
@12   60 07                      ; ethertype 0x6007 DEC SCA/LAVC (sec 2)
;
; DIFFERS: 0x0070+2 = 114 content, where the 0x05 revision carries 0x0076 -> 120.
; The 6-byte delta is exactly the abs 128-133 tail this revision does not have.
;
@14   70 00                      ; SCA length field (sec 2 identity: 14+114 = 128 = wire-len)
@16   ab 00 04 01 01 02          ; dst/group LOGICAL addr = the group (sec 4a)
;
; DIFFERS: 0x0101, where the 0x05 revision carries 0x0001. No meaning is
; claimed for either value; sec 4(a) records the field as an observed constant.
;
@22   01 01                      ; abs 22 connect-flag word
@24   aa 00 04 00 c5 07          ; src LOGICAL LAVC addr = aa:00:04:00:<LE16(1989)>, VAXC's SCSSYSTEMID
@30   a0 00                      ; per-frame word a0 = multicast (sec 4a, IDENTICAL to the 0x05 revision)
@32   08 00 00 80                ; discovery-family constant prefix (sec 4a, IDENTICAL)
;
; DIFFERS: the message-class byte is 0x03. Across the 75 000+ discovery frames
; of the clean-room corpus (five real V7.3 VAXes plus OVMX) abs 36 takes only
; 0x05 (HELLO) and 0x02 (SOLICIT); 0x03 appears zero times there and is the
; ONLY class this V5.5 node ever sends. No meaning is assigned to the value.
;
@36   03                         ; message-class byte
@37   01 00 00                   ; discovery-family constant suffix (sec 4a, IDENTICAL)
@40   06                         ; node-name length prefix (matches the name below)
@41   56 41 58 43 20 20          ; "VAXC  ", ASCII space-padded (sec 4a)
;
; The abs 47-67 discovery-format span (sec 4(a).2). Byte-identical to the V7.3
; corpus's span EXCEPT abs 63: 0x10 here, 0x18 there. No meaning is claimed for
; any of these bytes; OVMX never bakes them in -- vms_pe_fsm.c LEARNS them off
; the peer that really sent them.
;
@47   00 80 01 ff 83 00 04 00 00 00 00 00 00 00 00 00 10   ; abs 47..63
@64   03 00 00 00                ; abs 64..67
@68   00 00 00 00                ; join nonce: zero on a multicast HELLO (sec 4a, IDENTICAL)
@72   00 00 00 00 00 00 00 00 00 00
@82   00 00 00 00 00 00 00 00 00 00   ; abs 72..91 zero padding (sec 4b, IDENTICAL)
@92   00 00                      ; incarnation: 0 on a multicast HELLO (sec 4b, IDENTICAL)
;
; DIFFERS: wire bytes 90 05, where the 0x05 revision carries 92 05.
;
@94   90 05                      ; abs 94 trailer word
;
; abs 96..101 is the LIVE 48-bit tick -- the ONLY span that changes across all
; 53 captured frames from this node, and it walks monotonically upward, at the
; same offset and width as the 0x05 revision's. UNCITED by construction: a live
; field is never a fixture constant.
;
@102  bc 00 03 58 51 41 00 00 00 00   ; abs 102..111 constant tail (sec 4b) -- IDENTICAL, byte for byte
@112  00 00 00 00 00 00 00 00    ; abs 112..119 zero padding (sec 4b, IDENTICAL)
@120  52 54 00 00 00 0c          ; sender's real HW LAN MAC (sec 4b) -- matches abs 6
;
; DIFFERS: wire bytes 21 00, where the 0x05 revision carries 26 00. And the
; frame ENDS here: there is no abs 128..133 (poller-sweep / 0x0064 / 0x0000)
; tail in this revision, which is the whole of the 6-byte length delta at abs 14.
;
@126  21 00                      ; abs 126..127 trailer word, the last two bytes on the wire
