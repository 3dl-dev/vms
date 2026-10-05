# Real OpenVMS VAX FAL vs OVMX DAP (rd vms-a8a), 2026-10-04

These captures re-ground OVMX's DAP codec and FAL client on a real OpenVMS VAX
V7.3 FAL (DECnet object 17). They came from the DECnet lane's own isolated lab
pod `dnlab-1` (two stock V7.3 SIMH nodes on a private `br0`; VAX1 = 1.1). The
OVMX side was node 1.43 on a veth into the same bridge. Clean-room (Rule 8): this
records wire behaviour only. The DAP field layouts come from the public DAP 5.6
spec, AA-K177A-TK.

Passwords are redacted with same-length placeholders (`xxxxxx`, `yyyyyyy`), so
frame checksums are stale. The structure is intact.

## Files

| file | what |
|---|---|
| `fal-probe.pcap` / `.wire.txt` | `dapprobe.py`, an independent Python NSP + DAP instrument, against VAX1 FAL. Four runs: a wrong password (Disconnect reason 34), a missing file (STATUS 0x4032 FNF with STV 0x0910), a GET of a two-record file, and a PUT of two records. |
| `fal-ovmxclient.pcap` / `.wire.txt` | OVMX's **compiled** `dnet_fal_client_get` and `dnet_fal_client_put` (`src/vmsdecnet/fal/dnet_fal.c` + `src/vmsdecnet/dap/dnet_dap.c`) driving the same FAL. The Connect Initiate's session-control data comes from OVMX's `dnet_cterm_sc_connect_build`. `faldrv.c` pipes each NSP segment to `dapprobe.py`, which carries it over a real NSP link. Only the local-file side (`rms_textfile_*`) is stubbed onto stdio, because the bare pod has no executive. Both transfers completed with status 1: GET wrote `Hello line one` / `line two`, and PUT landed `FROMC.TXT`, which `TYPE` on VAX1 then printed verbatim. |
| `dapprobe.py`, `faldrv.c` | The lab instruments, kept so the run can be reproduced. Neither ships in any image. |

## What the wire showed

1. **Framing is the DAP 5.6 spec's.** The real FAL decodes OVMX's unblocked
   messages (FLAGS 0, operand to segment end). It sends its own replies blocked
   with `FLAGS.LENGTH`: ATTRIBUTES + NAME + ACK arrive in one segment, and DATA
   records arrive with LENGTH+LEN256. OVMX's receiver splits them. The previous
   codec's frame was OVMX's own design and could not work with a real FAL:
   FLAGS was always LENGTH, field widths were fixed, NAME was type 10, and
   OSTYPE 1 was used for VMS.
2. **An ATTRIBUTES message must come before ACCESS.** A bare ACCESS gets
   `STATUS 0xA003` (MAC 10 sync error, MIC = ACCESS).
3. **The FAL connect carries four counted fields:**
   `27 | user | password | account(00) | usrdata(00)`. This is also true of the
   vms-cd3 oracle frame (NSP length 63 covers both trailing zeros). An earlier
   reading counted three.
4. **DAP 7 extras appear only between DAP 7 peers.** VAX↔VAX DAP 7.2 segments
   (vms-cd3) end with 2 bytes the 5.6 spec does not define. With OVMX
   advertising DAP 5.6, the VAX sends none. The decoder bounds and ignores
   trailing operand bytes beyond the fields it knows, and tolerates ATTMENU
   bit 21.
5. **Status codes.** End of file is `0x5027` (MAC 5, MIC 047). File not found
   is `0x4032` (MAC 4, MIC 062) with STV `0x0910`. On the VAX,
   `F$MESSAGE(%X910)` is `%SYSTEM-W-NOSUCHFILE`. OVMX's `ssdef.h` says
   `SS$_NOSUCHFILE` = 2696, which real VMS reads as NOSUCHOBJECT (rd vms-ef2),
   so OVMX's FAL server omits STV until that is fixed.
