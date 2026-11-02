# vms-0a7 S1: DIRECTORY, TYPE and COPY node:: as RMS clients, against a real VAX V7.3 (2026-11-02)

rd vms-0a7, slice S1, branch `work/vms-0a7-s1-rms-remote`, image built from **b59e80288**.
In S1, DCL `DIRECTORY`, `TYPE` and `COPY` of `node"user pw"::spec` are plain RMS clients.
`src/vmsrms/rms_remote.c` does `$ASSIGN _NET:`, then `$QIOW IO$_ACCESS` with the NCB, then DAP to the
remote FAL. DCL no longer activates DECNETD.EXE. This run repeats the vms-6a4 matrix
(`../vms-6a4-dir-type-copy-20261102/`) against the same real VAX and adds more cases.

**Result: FAIL.** Small transfers work and match the oracle. A transfer that makes the VAX send a large
NSP data segment stalls: OVMX never acknowledges the segment, the VAX disconnects after about 190 s, and
the command fails. This affects a 36-file `DIRECTORY` (1a), which passed in vms-6a4, and a 2000-record
`COPY` from the VAX (3f).

## Bench

This is the vms-6a4 bench, copied into `bench/` and run locally on workshop. No lab-pod VMS node was used.
Container `vms0a7-vaxc`, multicast group 230.0.0.71:12471, SIMH UDP ports 41071/41072, work dir
`/tmp/vms0a7-lab`.

- **VAXC**: real OpenVMS VAX V7.3 under SIMH (`ovmx-vaxlab:5`). The disk is a scratch copy of the Node C
  DECnet disk `/lab/cluster-demo-nodeC-v73-decnet-r2/vms73-nodeC-decnet-cluster.dsk.gz` from the lab PVC,
  sha256 `974af57e43fa1302…` (the same disk vms-6a4 used). It runs DECnet 1.965. The console loads
  DVNETRTG, VAX-VMS and VAXCLUSTER from the lab PAKs (`/lab/cluster/PAKS.COM`). The console log is paused
  while they are typed, so the PAKs are fully redacted.
- **OVMXA**: x86 OVMX under KVM. The image comes from `git archive b59e80288` and
  `docker build -f distro/Dockerfile.bootable --target runner`. DECnet 1.963 is injected by
  `tools/cluster-web-demo/inject-decnet-config.sh --scsnode OVMXA` (PASS, read back).
- **Fixtures** (`bench/fal_verbs.py`): the same as vms-6a4: DNTEST [200,201], BRK1/BRK2 (DNTEST,
  `G:RE,W`) and BRKP ([1,4], `G,W`). The bench adds `BIG.TXT`, which DCL `OPEN/WRITE` writes as 2000
  records of 69 bytes. Note that this makes it VFC, not VAR/CR.
- **Wire**: frames from OVMXA's NIC are captured by qemu `filter-dump` and decoded by `bench/decode_nsp.py`
  into `wire.txt`, with passwords shown as XX. The pcap is not committed.
- **Passwords**: every password is replaced by `XX` in the committed logs. Before redaction, the raw OVMXA
  console was checked. The wrong password (`5a`/`5b`) and both good passwords appear only on the echoed
  `$ ` command lines (non-command occurrences: 0, 0, 0). OVMX prints the node as `VAXC"DNTEST password"::`.

## Results ($STATUS from `events.log`)

| # | OVMXA command | result | $STATUS | verdict |
|---|---|---|---|---|
| 1a | `DIRECTORY VAXC"SYSTEM pw"::SYS$LOGIN:*.COM` (36 files) | after 191 s: `%DIRECT-E-OPENIN, error opening VAXC"SYSTEM password"::SYS$LOGIN:*.COM;* as input` / `-SYSTEM-F-ABORT, abort` | %X1000002C | **FAIL** (passed in vms-6a4) |
| 1b | `DIRECTORY …SYS$LOGIN:NOSUCH*.XYZ` | `%DIRECT-W-NOFILES, no files found` | %X10018292 | PASS |
| 1c/1d | `DIRECTORY …BRK*.TXT` as SYSTEM and as DNTEST | BRK1, BRK2, BRKP, `Total of 3 files.` | %X00000001 | PASS |
| 1e | `DIRECTORY VAXC"DNTEST pw"::SYS$LOGIN:NOSUCH.TXT` | `%DIRECT-W-NOFILES` | %X10018292 | PASS (text); $STATUS differs from VMS |
| 1f | `DIRECTORY/FULL VAXC"DNTEST pw"::…` | `%SYSTEM-F-UNSUPPORTED`, as in vms-6a4 | %X00000E4A | not in scope (honest refusal) |
| 2a | `TYPE VAXC"SYSTEM pw"::…BRK1.TXT` | both records | %X00000001 | PASS |
| 2b | `TYPE VAXC"SYSTEM pw"::…BRK*.TXT` | three files, each with its header | %X00000001 | PASS |
| 2c/2d | `TYPE VAXC"DNTEST pw"::SYS$LOGIN:BRK*.TXT` / `BRKP.TXT` | BRK1 and BRK2 typed; BRKP gives `%TYPE-W-OPENIN …` / `-RMS-E-PRV` | %X1001829A | PASS (text); $STATUS differs from VMS |
| 5a | `DIRECTORY VAXC"DNTEST wrong"::SYS$LOGIN:BRK*.TXT` | `%DIRECT-E-OPENIN …` / `-RMS-E-FND` / `-SYSTEM-F-INVLOGIN`; the password is not echoed | %X1000209C | PASS (text identical to VMS); $STATUS differs |
| 5b | `TYPE VAXC"DNTEST wrong"::SYS$LOGIN:BRK1.TXT` | `%TYPE-W-SEARCHFAIL …` / `-RMS-E-FND` / `-SYSTEM-F-INVLOGIN`; the password is not echoed | %X1000209C | PASS (text identical to VMS); $STATUS differs |
| 3a–3c | `COPY` BRK1.TXT VAX→OVMX, `TYPE` it, `COPY` it back OVMX→VAX as BRK1RT.TXT | on VAXC: `DIFFERENCES` gives 0 sections and 0 records; `CHECKSUM` is 742093723 for both; `DIRECTORY/FULL` gives Variable length, Carriage return carriage control and the same protection for both | %X00000001 | **PASS** |
| 3d | `COPY/LOG VAXC"SYSTEM pw"::…BRK*.TXT SYS$LOGIN:*.*` | 4 `%COPY-S-COPIED` lines and `%COPY-S-NEWFILES, 4 files created` | %X00000001 | PASS. VMS also prints ` (1 block)` on each line |
| 3f | `COPY VAXC"SYSTEM pw"::…BIG.TXT SYS$LOGIN:BIG.TXT` (2000 records, 290 blocks) | after 190 s: `%COPY-E-READERR, error reading VAXC"SYSTEM password"::SYS$SYSDEVICE:[SYSMGR]BIG.TXT;1` / `-RMS-F-RER, file read error`. It leaves an empty BIG.TXT on OVMXA | %X1001C0F4 | **FAIL** |
| 3h | `COPY SYS$LOGIN:BIG.TXT VAXC"…"::…BIGRT.TXT` | copies the empty BIG.TXT that 3f left. VAXC: `DIFFERENCES` BIG vs BIGRT gives 1 section and 2000 records; CHECKSUM 33952600 vs 0 | %X00000001 | consequence of 3f |
| 4a/4b | VAXC `COPY V2O.TXT OVMXA"SYSTEM pw"::`, then back as V2O_BACK.TXT | `DIFFERENCES` gives 0 sections; CHECKSUM 2033922319 for both; both VAR/CR | %X00000001 | **PASS** |
| 4f/4g | VAXC `DIRECTORY OVMXA"SYSTEM pw"::` / `…V2O.TXT` | 22 files listed from OVMXA's `SYS$SYSROOT:[SYSMGR]` | %X00000001 | PASS |
| 0 | DECNETD.EXE not activated | **not proven at run time**; see below | | open |

### The stall, on the wire (`wire.txt`)

The 1a link is OVMXA 0xe45a ↔ VAXC 0x2001. At t=6.383 VAXC sends DAP data segment 2 (1067 bytes,
`nsp 02 80 02 10 …SYS$SYSROOT:…[SYSMGR…`). OVMXA never sends a DATA-ACK for it. VAXC retransmits it
every 19 s (25.062, 44.062, … 177.291). At 196.316 it sends `NSP DI … reason 38`, and OVMXA then reports
`-SYSTEM-F-ABORT`. The 3f link (0xe476 ↔ 0x201d) shows the same thing.
VAXC sends a burst of 5 full 1475-byte segments at 205.899. None is ACKed, they are retransmitted, and
at 395.566 VAXC sends `NSP DI … reason 38`.
The smaller inbound segments in these links are ACKed. Transfers in which OVMX sends the data (3c, 3h,
and 4b, where the VAX reads from OVMX's FAL) are not affected. No BIG.TXT record reaches OVMX.

### DECNETD.EXE not activated: not proven at run time

The bench tried to remove DECNETD.EXE from SYS$SYSTEM during OVMXA's client commands. OVMX's RENAME
refused, although DIRECTORY finds the file (`ovmxa-console.log`):

```
$ DIRECTORY/SIZE SYS$SYSTEM:DECNETD.EXE
DECNETD.EXE;1                             2084
$ RENAME SYS$SYSTEM:DECNETD.EXE SYS$SYSTEM:DECNETD.S1AWAY
%RMS-E-RNF, file not found - SYS$SYSTEM:DECNETD.EXE        ($STATUS %X00000910)
```

This RENAME failure is a separate bug, outside S1. OVMX has no runtime counterpart to JPI$_IMAGNAME for a
running image, and DCL's Ctrl/T and SHOW PROCESS/ACCOUNTING carry no image count, so this run has no
runtime evidence about image activation. The only evidence is the source: at b59e80288,
`src/vmsdcl` names DECNETD only for SET HOST (`dcl_cmd_set.c`). `SHOW SYSTEM` before the client commands, at 3g
(after the failed long COPY) and after them lists the same five processes (OPCOM, NETACP, STARTUP, SYSTEM,
JOB_CONTROL). Because the old path activated the image in the caller's own process, this does not
discriminate.

## Against the oracles (`oracle-diff.txt`, `bench/oracle_diff.py`)

**(A) The VAX1→VAX2 oracle** (`../decnet-fal-verbs-20261008/vax-to-vax-sys-login/`, node string normalised,
as in vms-6a4): `DIRECTORY …BRK*.TXT`, `DIRECTORY …NOSUCH.TXT` and `TYPE …BRK*.TXT` (with the BRKP
`-RMS-E-PRV` refusal) are **identical** byte for byte. `DIRECTORY/FULL` is different
(`%SYSTEM-F-UNSUPPORTED`), as in vms-6a4.

**(B) A same-run VAX loopback, new in this run.** VAXC's own DCL typed the same command lines against
VAXC's FAL, with the same accounts and access strings. Output text:
- Wrong password: `DIRECTORY` (OPENIN/FND/INVLOGIN) and `TYPE` (SEARCHFAIL/FND/INVLOGIN) are **identical**,
  including VMS's `VAXC"DNTEST password"::` rendering. The loopback ran after OVMXA's COPY 3c had created
  BRK1RT.TXT, so it lists one more file. Apart from that line, `DIRECTORY BRK*`, `NOSUCH`, `TYPE BRKP`
  and `TYPE BRK*` are identical.
- `COPY/LOG`: VMS ends each `%COPY-S-COPIED` line with ` (1 block)`. OVMX does not.
- **$STATUS differs** in every error case:

| case | VAX V7.3 | OVMX |
|---|---|---|
| DIRECTORY NOFILES | %X10018290 (W) | %X10018292 (E) |
| TYPE OPENIN/PRV | %X10951098 (TYPE facility) | %X1001829A |
| DIRECTORY, wrong password | %X1001C02A | %X1000209C (bare SS$_INVLOGIN) |
| TYPE, wrong password | %X10951238 | %X1000209C |

## Files

`ovmxa-console.log`, `vaxc-console.log`, `events.log`, `wire.txt` (passwords XX), `oracle-diff.txt`, and
`bench/` (`fal_verbs.py`, `oracle_diff.py`, `decode_nsp.py`, `node.py`, `udp2mcast.py`, `vaxc.ini`).
