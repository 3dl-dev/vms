# vms-0a7 S2: DCL SET HOST through SYS$SYSTEM:RTPAD.EXE, against a real VAX V7.3 (2026-11-03)

rd vms-0a7, slice S2, branch `work/vms-0a7-s2-rtpad`. DCL `SET HOST node` activates
`SYS$SYSTEM:RTPAD.EXE`. RTPAD reads NODE and /USERNAME with CLI$GET_VALUE, opens the CTERM link
with `$ASSIGN _NET:` and `$QIOW IO$_ACCESS`, and carries the terminal over `$QIO` on `_NET:` and on
`TT:`. DECNETD.EXE is not involved. The logs in this directory are from the image built from
**dc52ca31c** (`git archive dc52ca31c`, `docker build -f distro/Dockerfile.bootable --target runner`).
Later commits on the branch change only this directory, comments and tests.

**Result.** Every case listed under "Matches the VAX" below behaves as the VAX's own SET HOST does.
The open differences are in their own table, and each has an rd item.

## Bench

`bench/`, run locally on workshop. It is based on the S1 bench (`../vms-0a7-s1-20261102/bench/`),
with its own names: container `vms0a7s2-vaxc`, multicast 230.0.0.83:12483, SIMH UDP 41081/41082,
work dir `/tmp/vms0a7s2-lab`. `bench/run.sh` restores both disks from pristine copies before each run.

- **VAXC**: a real OpenVMS VAX V7.3 under SIMH (`ovmx-vaxlab:5`), DECnet 1.965. The disk is a
  scratch copy of `/lab/cluster-demo-nodeC-v73-decnet-r2/vms73-nodeC-decnet-cluster.dsk.gz` (gz
  sha256 `974af57e43fa1302…`, raw `4113c11b108329ba…`). DVNETRTG, VAX-VMS and VAXCLUSTER are
  registered from the lab PAKs while the console log is paused, so the PAKs never appear.
  `REPLY/DISABLE` keeps OPCOM's security alarms off the transcript.
- **OVMXA**: x86 OVMX under KVM, DECnet 1.963, configured by
  `tools/cluster-web-demo/inject-decnet-config.sh --scsnode OVMXA` (PASS). The QEMU console is
  `-chardev stdio,signal=off`, so a typed CTRL/C reaches the guest and does not stop QEMU.
- **The oracle**: the same case list run by the VAX's own SET HOST in the same session.
  `SET HOST 0` (VAX→VAX) is the oracle for OVMX→VAX. `SET HOST OVMXA` (VAX→OVMX) is the oracle for
  what a VAX *client* puts on the wire.
- **Wire**: qemu `filter-dump` on OVMXA's NIC. `wire.txt` is the NSP decode (`bench/decode_nsp.py`).
  `cterm.txt` lists every CTERM Start Read, Read Data, Out-of-Band and Characteristics message
  (`bench/cterm_reads.py`). The pcap is not committed. The only links on the wire are those that
  involve OVMXA; `SET HOST 0` stays inside its own node.
- **Passwords**: `bench/redact.py` replaces every bench password with `XX` in every committed file,
  both as text and as decoder hex, and that was checked for each file. The bench reads the passwords
  from the environment.

Files: `events.log` (one line per case, with timestamps), `cases.log` (what the screen showed for
each case), `ovmxa-console.log`, `vaxc-console.log`, `wire.txt`, `cterm.txt`, `proof-no-decnetd/`,
`baseline-37faf679e/` (the same bench on the branch before the fixes) and `bench/`.

## Results (image dc52ca31c)

The OVMX→VAX case is compared with VAX→VAX (`SET HOST 0` on VAXC). $STATUS values come from
`SHOW SYMBOL $STATUS` on the local node.

### Matches the VAX

| case | OVMX `SET HOST VAXC` | VAX `SET HOST 0` | $STATUS OVMX / VAX | verdict |
|---|---|---|---|---|
| connect | banner, `Username:` after 0.4 s | banner, `Username:` after 0.1 s | — | PASS |
| wrong password | `User authorization failure`, then `%REM-S-END, control returned to node OVMXA::` after **20.2 s** | the same after 19.6 s | — | **PASS** (baseline: hung for ever) |
| login | the banner, last login, `1 failure since last successful login` | the same | — | PASS |
| prompts | `Username: SYSTEM` / `Password:` / `$ ` with no blank lines | the same | — | **PASS** (baseline: a blank line before every prompt) |
| node, terminal | `VAXC _VAXC$RTA1:`; Remote Port Info `OVMXA::SYSTEM` | `VAXC _VAXC$RTA1:`; `VAXC::SYSTEM` | — | PASS |
| SHOW SYSTEM, SHOW USERS, DIRECTORY | the VAX's process list, the user list and `Directory SYS$COMMON:[SYSEXE]` | the same | — | PASS |
| line editing: DELETE, CTRL/U | `SHOW TIMX`⌫`E` runs SHOW TIME; CTRL/U clears the line and the prompt is shown again | the same. The VAX erases with ANSI sequences on its VT100; OVMXA's console type is unknown | — | PASS |
| recall (up arrow) | the recalled `SHOW TIME` runs | the same | — | PASS (display differs, see below) |
| CTRL/Y during a remote SEARCH | `*INTERRUPT*`; the SEARCH stops; the session goes on (`NODE=VAXC`) | ` Interrupt `; the SEARCH stops; the session goes on | — | **PASS** (baseline: RTPAD was killed, the remote session left orphaned) |
| CTRL/C during a remote SEARCH | the same as CTRL/Y | the same as CTRL/Y | — | **PASS** (baseline: as CTRL/Y) |
| LOGOUT | `logged out at …` / `%REM-S-END, control returned to node OVMXA::`; back in OVMX DCL | `… node VAXC::` | %X00000001 / %X00000001 | PASS |
| unknown node | `SET HOST NOSUCH`: `%SYSTEM-F-NOSUCHNODE, remote node is unknown` | the same | **%X0000028C / %X0000028C** | **PASS** (baseline: %X1000028C) |
| bad qualifier | `SET HOST VAXC /BOGUS`: `%DCL-W-IVQUAL … \BOGUS\` | the same | %X00038240 / %X00038240 | PASS |
| no DECNETD.EXE | `proof-no-decnetd/`: with `SYS$SYSTEM:` (for the process) a directory that holds only RTPAD.EXE, `RUN SYS$SYSTEM:DECNETD.EXE` gives `%DCL-E-IVIMAGE` (control), and `SET HOST VAXC` logs in, runs, and logs out with `%REM-S-END` | — | %X00000910 (control) / %X00000001 | **PASS** |

`SET HOST 0` on OVMXA (OVMX→OVMX) and the VAX's `SET HOST OVMXA` also log in, run every case and
log out with `%REM-S-END`. Differences on the OVMX *host* side are listed below.

### On the wire (`cterm.txt`)

- **Timed reads.** The VAX's LOGINOUT reads are timed (Start Read Q bit, `timeout 20`). After the
  wrong password the VAX issues a no-prompt retry read. Twenty seconds later RTPAD answers it with
  Read Data completion code 5 (`03 05 00 00 00 00 00 00`, t=34.069), and the VAX ends the session,
  as it does for its own client.
- **Out of band.** At login the VAX sends `0b 00 02 02 19 3b 29` (CTRL/Y out of band). RTPAD sends
  CTRL/Y, and CTRL/C as CTRL/Y, as `04 01 19`. A VAX client talking to OVMXA sends `04 00 19` for
  both keys, although OVMX never makes them out of band (t=155–171). RTPAD follows that: CTRL/C
  becomes CTRL/Y, and CTRL/Y always goes out of band. The discard flag in `04 01` follows the VAX
  host's attribute bit 3. That bit is not pinned by an oracle, but the VAX acts on the message.

### Open differences (each filed)

| case | OVMX | VAX | rd |
|---|---|---|---|
| CTRL/Y at an idle remote prompt | `*INTERRUPT*`; RETURN goes on | ` Interrupt `, then RTPAD asks `Are you repeating ^Y to abort the remote session on node 0?`; RETURN goes on | vms-ec42 |
| 80-character line arriving at once (a paste) at the remote prompt | cut at the 78-character type-ahead buffer; CTRL/U and RETURN recover | read in full (VMS has the read outstanding while you type) | vms-fc0 |
| host output while a read is outstanding | shown when the read ends | shown at once | vms-fc0 |
| recall display | the driver echoes `^B`; the recalled line appears on a new line | redrawn in place | vms-2a0b |
| remote SHOW TERMINAL | `Device_Type: Unknown`, the oracle's characteristics (Hardcopy, No Tab) | VT100, Scope, Tab | vms-686 (OVMX SENSEMODE returns DEVCHAR, and SET TERMINAL/DEVICE leaves type 0) |
| OVMX as the host (VAX→OVMXA, OVMX→OVMX) | wrong password: `Username:` again at once; host writes `*INTERRUPT*`; a VAX client sees `Device_Type:` blank | 20-s timed retry, then end; host writes nothing for the interrupt; VT100 | vms-301 |
| `RENAME SYS$SYSTEM:DECNETD.EXE …` (an early proof attempt) | `%RMS-E-RNF` although DIRECTORY lists the file | — | vms-518 |

### Baseline (`baseline-37faf679e/`, the branch before these fixes)

With the image from 37faf679e:
- a wrong password hung the session;
- every prompt had a blank line before it;
- CTRL/Y killed RTPAD (`%DCL-F-ABORT … RTPAD.EXE terminated abnormally (signal 2)`) and left the
  remote session orphaned;
- the recalled command was lost;
- the remote saw the oracle's LA36 terminal at 132 columns;
- `SET HOST NOSUCH` left $STATUS %X1000028C.

Each fix is a commit on the branch, with host tests in `tests/vmsdecnet/test_rtpad_session.c`
(timed read, Out-of-Band, characteristics, recall, terminal image, the prompt after an interrupt)
and `tests/qemu/test_kmod_tt.c` (CTRL/Y and CTRL/C in an OUTBAND mask go to the mask owner).
`decnet_sethost_pair_boot_e2e` passes 19/19 on the dc52ca31c image.
