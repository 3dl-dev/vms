# A real OpenVMS VAX `SET HOST` into OVMX's CTERM host (2026-10-05)

This is the inbound direction of `SET HOST` for the DECnet lane (rd vms-a70 direction B), captured on the lane's lab pod `dnlab-1`. VAX1 (1.1) is a stock OpenVMS VAX V7.3 node with DECnet. Its `SET HOST` was pointed at node 1.44.

At 1.44, `ctermprobe.py` carried the NSP logical link. It answered the Connect Initiate to object 42 and piped every data segment to `ctermdrv`. `ctermdrv` runs OVMX's compiled CTERM host FSM, `src/vmsdecnet/cterm/dnet_cterm_hostfsm.c`. This is the same code NETACP runs for object 42.

The session's terminal is a pseudo-terminal running `ctlogin.sh`. That script is a lab stand-in: it shows prompts, reads lines and exits on `LOGOUT`. It checks no credential. In the product, the terminal is an executive-minted RTAn:, and the session is the real LOGINOUT.EXE created with `$CREPRC` (`dnet_cterm_host_open_desc`, unchanged). Only the wire protocol is under test here.

The password typed on VAX1 was a dummy (`notreal`), so the capture holds no real credential.

## What VAX1 showed (`vax1-console.txt`)

```
$ SET HOST 1.44

  OVMX CTERM host (lab stand-in session, node 1.44)

Username: SYSTEM
Password:

  Lab stand-in: user SYSTEM, password of 7 characters received (not checked)

$ SHOW TIME from VAX1
  OVMX host received 19 characters: "SHOW TIME from VAX1"

$ LOGOUT
  SYSTEM  logged out at 05-OCT-2026 01:15:00

%REM-S-END, control returned to node VAX1::
$
```

| Proof point | Where it shows |
|---|---|
| The VAX shows the host's prompt | `Username:` arrives as the prompt of the host's Start Read (TX seg 6) |
| Typed lines reach the host | Read Data `SYSTEM\r`, `SHOW TIME from VAX1\r`, `LOGOUT\r` (RX seg 5, 7, 8; `probe.log` "typed-line") |
| No-echo read | the host saw echo off on its terminal and sent the Password: read with the no-echo flag (`02 08 98 00`); VAX1 did not echo the password |
| Host output appears on the VAX | the Writes in TX seg 5, 8, 10 and 12 appear on the VAX1 console |
| Logout ends the session | the host sends Unbind `02 03 00` (TX seg 13), and VAX1 prints `%REM-S-END` |

## The exchange (`sethost-inbound.wire.txt`, decoded from `sethost-inbound.pcap`)

1. CI to object 42 from 1.1 with source user `SYSTEM` and empty access control. The probe answers with CC.
2. **The host speaks first:** Bind Request `01 02 04 00 07 00 10 00`. These are the bytes a VAX host sends.
3. VAX1 answers with Bind Accept `04 02 04 00 07 00 ...`.
4. The host sends Common Data {CTERM Initiate, Characteristics `08 02 = 2`}, then Common Data {Characteristics `02 02 03 3b 00`}. These match the VAX host's bytes except for one parameter: the Initiate advertises OVMX's true maximum message size, `fc 03` (1020), where a VAX host sends `10 1e`.
5. VAX1 sends its Initiate, VMS message 23 and VMS message 19. The host answers with message 23, as a VAX host does.
6. From here the host runs terminal I/O. Output goes as raw Writes (`07 32 00 00 00 <data>`). Each prompt rides in a Start Read: flags `08 90 00`, or `08 98 00` for a no-echo read. VAX1 returns each line as Read Data `03 00 ...`.
7. When the session process exits, the host sends its last output, then Unbind, then the NSP disconnect.

## Files

| File | What it is |
|---|---|
| `sethost-inbound.pcap` | the DECnet wire on the pod's `br0` (ethertype 0x6003) |
| `sethost-inbound.wire.txt` | every NSP data segment, decoded by `nspdump.py` |
| `vax1-console.txt` | VAX1's console for the session |
| `probe.log` | the probe and driver log; the password line is logged as `<redacted>` |
| `ctermprobe.py` | NSP lab instrument (derived from the FAL lane's `dapprobe.py`) |
| `ctermdrv.c` | links the compiled host FSM to a pty; built `musl-gcc -static` from the real sources |
| `ctlogin.sh` | the stand-in session script |
| `nspdump.py`, `gen_oracle_inc.py` | the decoder, and the generator of `tests/vmsdecnet/cterm_host_oracle.inc` |

## Reproduce

On workshop, from this directory:

```
musl-gcc -static -O2 -I../../../../src/vmsdecnet/cterm/include ctermdrv.c \
    ../../../../src/vmsdecnet/cterm/dnet_cterm_hostfsm.c -o /tmp/ctermdrv
```

Copy `ctermdrv`, `ctermprobe.py` and `ctlogin.sh` into the pod's `/tmp`. Then, as root in the pod:

```
tcpdump -i br0 -U -w /tmp/cti.pcap ether proto 0x6003 &
ROUTER=1 DRV=/tmp/ctermdrv python3 /tmp/ctermprobe.py dnp0 1.44 /tmp/ctlogin.sh
```

Wait one hello interval (about 10 s) so VAX1 can hear 1.44. Then type `SET HOST 1.44` on VAX1.

## Not proven here

This run does not cover the RTAn: and LOGINOUT half. That half is unchanged, and the booted acceptance battery proves it (`--cterm-accept-test`). A real VAX `SET HOST` into a **booted** OVMX node, with both halves live together, is still open.
