# vms-0a7 S1: DIRECTORY, TYPE and COPY node:: as RMS clients, against a real VAX V7.3 (2026-11-02)

rd vms-0a7, slice S1, branch `work/vms-0a7-s1-rms-remote`. The logs in this directory come from the
image built from **2c7903b26** (run 3, below). In S1, DCL `DIRECTORY`, `TYPE` and `COPY` of
`node"user pw"::spec` are plain RMS clients. `src/vmsrms/rms_remote.c` does `$ASSIGN _NET:`, then
`$QIOW IO$_ACCESS` with the NCB, then DAP to the remote FAL. DCL does not activate DECNETD.EXE for them.
This repeats the vms-6a4 matrix (`../vms-6a4-dir-type-copy-20261102/`) against the same real VAX and
adds cases.

**Result: FAIL.**
- Every small-file case passes. A 36-file DIRECTORY passes, and the DECNETD.EXE control passes.
- Large files fail. A large file read from the VAX is cut off when OVMX disconnects the link (NSP DI
  reason 1). A large file sent from OVMX to the VAX hangs.

## Runs

| run | image | what happened |
|---|---|---|
| 1 | b59e80288 | Logs in commit 3c07ad281. `DIRECTORY …*.COM` (36 files) and a 2000-record COPY stalled. OVMX never ACKed VAX segments of 1067 and 1475 bytes. The VAX retransmitted for 190 s, then sent DI reason 38. Fixed by 571fab374 (DNET_NSP_MAX_DATA 1024 → 1459). |
| 2 | 2c7903b26 | The bench itself failed in DIFFERENCES on a 20000-record file; the bench timed out. Results up to that point are the same as run 3. Not committed. |
| 3 | 2c7903b26 | **These logs.** The bench stopped at step 7c (OVMX → VAX COPY of a large file, which hung, see below) after 900 s. Every step before it completed. |

## Bench

This is the vms-6a4 bench, copied into `bench/` and run locally on workshop. No lab-pod VMS node was used.
Container `vms0a7-vaxc`, multicast 230.0.0.71:12471, SIMH UDP 41071/41072, work dir `/tmp/vms0a7-lab`.

- **VAXC**: real OpenVMS VAX V7.3 under SIMH (`ovmx-vaxlab:5`). The disk is a scratch copy of
  `/lab/cluster-demo-nodeC-v73-decnet-r2/vms73-nodeC-decnet-cluster.dsk.gz` from the lab PVC, sha256
  `974af57e43fa1302…`. It runs DECnet 1.965. DVNETRTG, VAX-VMS and VAXCLUSTER are loaded from the lab
  PAKs. The console log is paused while they are typed, so the PAKs are fully redacted.
- **OVMXA**: x86 OVMX under KVM. The image comes from `git archive 2c7903b26` and
  `docker build -f distro/Dockerfile.bootable --target runner`. DECnet 1.963 is injected by
  `tools/cluster-web-demo/inject-decnet-config.sh --scsnode OVMXA` (PASS). Each run starts from a fresh disk.
- **Fixtures** (`bench/fal_verbs.py`): DNTEST [200,201], BRK1/BRK2 (DNTEST, `G:RE,W`) and BRKP ([1,4],
  `G,W`), as in vms-6a4. Two larger files are added, both written by DCL `OPEN/WRITE`:
  - `BIG.TXT`, 2000 records, 290 blocks;
  - `LONG.TXT`, 20000 records, 3125 blocks.

  Both are VFC with print-file carriage control. BRK* are VAR/CR.
- **Wire**: frames from OVMXA's NIC are captured by qemu `filter-dump` and decoded by `bench/decode_nsp.py`
  into `wire.txt`, passwords XX. The pcap is not committed.
- **Passwords**: they are replaced by `XX` in all committed files. Before redaction, the raw consoles
  were checked. The good passwords and the wrong one appear only on echoed `$ ` command lines, 0 times
  elsewhere on either console. Both OVMX and VMS print the node as `VAXC"DNTEST password"::`.

## Results (run 3, $STATUS from `events.log`)

| # | command (on OVMXA unless VAXC) | result | $STATUS | verdict |
|---|---|---|---|---|
| 0a–0b | `DEFINE/PROCESS SYS$SYSTEM SYS$LOGIN:`, then `SET HOST VAXC` | `DIRECTORY SYS$SYSTEM:DECNETD.EXE` gives NOFILES. SET HOST, which still activates DECNETD.EXE on this branch, gives `%SET-I-NOTAVAIL, DECnet is not available on this system` | %X00000001 | control is valid |
| 0c–0f | with that logical still set: `DIRECTORY …BRK*.TXT`, `TYPE …BRK1.TXT`, `COPY` BRK2 VAX→OVMX→VAX as BRK2RT | all work. On VAXC: DIFFERENCES 0, CHECKSUM 1225656911 for both, both VAR/CR | %X00000001 | **PASS**: no DECNETD activation |
| 1a | `DIRECTORY VAXC"SYSTEM pw"::SYS$LOGIN:*.COM` | both directories of the search list: `SYS$SYSROOT:[SYSMGR]` (the bench's MK*.COM, 6 files) and `SYS$COMMON:[SYSMGR]` (36 files) | %X00000001 | **PASS** (failed in run 1) |
| 1b/1e | `DIRECTORY …NOSUCH*` | `%DIRECT-W-NOFILES` | %X10018290 | PASS, the same as the VAX |
| 1c/1d | `DIRECTORY …BRK*.TXT` as SYSTEM and as DNTEST | the full file list | %X00000001 | PASS |
| 1f | `DIRECTORY/FULL VAXC"DNTEST pw"::…` | `%SYSTEM-F-UNSUPPORTED` | %X00000E4A | honest refusal (not implemented) |
| 2a/2b | `TYPE` one file, and a wildcard as SYSTEM | the records, with a header per file | %X00000001 | PASS |
| 2c/2d | `TYPE VAXC"DNTEST pw"::…BRK*.TXT` / `BRKP.TXT` | the readable files are typed. BRKP gives `%TYPE-W-OPENIN …` / `-RMS-E-PRV` | %X1001829A | PASS (text) |
| 5a | `DIRECTORY VAXC"DNTEST wrong"::…` | `%DIRECT-E-OPENIN` / `-RMS-E-FND` / `-SYSTEM-F-INVLOGIN`. The password is not echoed | %X1000209C | PASS (text identical to VMS) |
| 5b | `TYPE VAXC"DNTEST wrong"::…` | `%TYPE-W-SEARCHFAIL` / `-RMS-E-FND` / `-SYSTEM-F-INVLOGIN`. The password is not echoed | %X1000209C | PASS (text identical to VMS) |
| 3a–3c | COPY BRK1 VAX→OVMX, then OVMX→VAX as BRK1RT | on VAXC: DIFFERENCES 0 sections; CHECKSUM 742093723 for both; both `Variable length` / `Carriage return carriage control` | %X00000001 | **PASS** |
| 3d | `COPY/LOG VAXC"SYSTEM pw"::…BRK*.TXT SYS$LOGIN:*.*` | 5 `%COPY-S-COPIED` lines and `%COPY-S-NEWFILES` | %X00000001 | PASS. VMS also prints ` (1 block)` |
| 3f, 0g | `COPY VAXC"…"::…BIG.TXT SYS$LOGIN:` (2000 records) | `%COPY-E-READERR, error reading VAXC"SYSTEM password"::SYS$SYSDEVICE:[SYSMGR]BIG.TXT;1` / `-RMS-F-RER, file read error`. It leaves a partial BIG.TXT (3 blocks) on OVMXA | %X1001C0F4 | **FAIL** |
| 3h | COPY the partial BIG.TXT back to VAXC as BIGRT | VAXC: BIGRT is 3 blocks against BIG's 290; DIFFERENCES 1 section; CHECKSUM 424506611 against 33952600 | %X00000001 | follows from 3f |
| 6a | `COPY VAXC"…"::…LONG.TXT SYS$LOGIN:` (20000 records) | the same READERR / RER | %X1001C0F4 | **FAIL** |
| 6b | COPY the partial LONG.TXT back to VAXC as LONGRT | 3 blocks against 3125; CHECKSUM 240197142 against 2 | %X00000001 | follows from 6a |
| 4a/4b | VAXC `COPY V2O.TXT OVMXA"SYSTEM pw"::`, then back as V2O_BACK.TXT | DIFFERENCES 0; CHECKSUM 2033922319 for both; both VAR/CR | %X00000001 | **PASS** |
| 4f/4g | VAXC `DIRECTORY OVMXA"SYSTEM pw"::` / `…V2O.TXT` | OVMXA's `SYS$SYSROOT:[SYSMGR]` is listed | %X00000001 | PASS |
| 7a | VAXC `COPY SYS$SYSDEVICE:[SYSMGR]BIG.TXT OVMXA"SYSTEM pw"::BIGV.TXT` (into OVMX's FAL) | succeeds after 157 s on the wire (link 69.3 s → 226.4 s). OVMXA `DIRECTORY/SIZE` shows 297 blocks. Identity is not checked, because 7c hung | %X00000001 | completes; slow |
| 7c | `COPY SYS$LOGIN:BIGV.TXT VAXC"SYSTEM pw"::…BIGVRT.TXT` (OVMX→VAX, large) | **hangs**: no prompt within 900 s, and the bench gave up | none | **FAIL** |

### The two large-file failures, on the wire (`wire.txt`)

**VAX → OVMX: OVMX aborts the link with NSP DI reason 1** (3f, 0g and 6a). OVMX's CI offers `services
0x01`, which means no NSP flow control. The VAX sends one DAP DATA message per record, 19 records per
1475-byte segment, and OVMX ACKs each segment. About 37 segments in, the VAX sends five segments back to
back. OVMX ACKs them and then sends `NSP DI … reason 1` on the same link. In run 3 this happens at
18.851 s (0g), 39.750 s (3f) and 44.562 s (6a). RMS then returns `-RMS-F-RER`. The DI follows a segment that starts with
record 0628 (0g) or 0666 (3f) of BIG.TXT, and record 00579 of LONG.TXT (6a).

**OVMX → VAX: OVMX ignores the VAX's flow control and never retransmits** (7c, link OVMXA 0xc961 ↔
VAXC 0x204b).
- OVMX sends one record per segment.
- At 227.016 the VAX sends link service `00 80 02 00 01 00`: LSFLAGS 1, which says stop sending data.
  OVMX goes on sending (227.017 onward).
- At 227.021 the VAX sends `00 80 03 00 02 00`: LSFLAGS 2, which says resume.
- The VAX's last DATA-ACK carries acknum `0x900e`, a NAK at segment 14. After it the VAX ACKs nothing
  more.
- OVMX goes on to send all 2000 records and the DAP access-complete (`07 00 01`, segment 0x7d6, at
  237.245). It never retransmits.
- From then on the link only carries the VAX's 60-second link-service keepalives and OVMX's OTHER-ACKs,
  until the bench stopped. The COPY never completes.

Small files never hit either path: BRK*, V2O and the 36-file directory listing all pass.

### DECNETD.EXE not activated (run-time proof)

Rows 0a–0f are the proof. A process-level logical makes `SYS$SYSTEM:` mean `SYS$LOGIN:` for the console
process only. NETACP is another process, already running from DECNETD.EXE, so the network stays up.
- **Positive control.** With the logical set, DCL `SET HOST VAXC`, which on this branch still resolves
  and activates `SYS$SYSTEM:DECNETD.EXE` (`src/vmsdcl/dcl_cmd_set.c`), reports
  `%SET-I-NOTAVAIL, DECnet is not available on this system`. The logical therefore does block activation
  of DECNETD.EXE from this process.
- **Test.** With the same logical in place, DIRECTORY, TYPE and COPY (both ways) of `VAXC"…"::` all work.
  The BRK2 round trip is byte-identical on VAXC.

**Second login (6a).** VAXC ran `SET HOST OVMXA` and logged in as `_RTA0:`, PID 10000007, then sampled
console PID 10000006 three times. `SHOW SYSTEM` lists the same six processes each time, with no
DECNETD/FAL process. `SHOW PROCESS/ID=10000006` shows the console process. This sampling does not prove
anything on its own, for two reasons:
- the 20000-record COPY failed with DI reason 1 within seconds, so it may already have ended when the
  samples were taken;
- the pre-S1 path activated DECNETD.EXE in the caller's own process.

`WRITE SYS$OUTPUT "IMAGNAME=<" + F$GETJPI("10000006","IMAGNAME") + ">"` on the CTERM session printed
**nothing**, not even the literal text. That is a separate anomaly and was not investigated. OVMX has no
image count in SHOW PROCESS/ACCOUNTING.

## Against the oracles (`oracle-diff.txt`, `bench/oracle_diff.py`)

**(A) The VAX1→VAX2 oracle** (`../decnet-fal-verbs-20261008/vax-to-vax-sys-login/`, node string normalised):
- `DIRECTORY …NOSUCH.TXT` is identical.
- `DIRECTORY …BRK*.TXT` and `TYPE …BRK*.TXT` (with the BRKP `-RMS-E-PRV` refusal) differ only by one
  file, BRK2RT.TXT, which control 0f created in the fixture directory.
- `DIRECTORY/FULL` differs (`%SYSTEM-F-UNSUPPORTED`).

**(B) Same-run VAX loopback.** VAXC's own DCL typed the same command lines against VAXC's FAL. Output
text:
- Identical: `NOSUCH`, `TYPE BRKP`, wrong-password `DIRECTORY`, wrong-password `TYPE`.
- `DIRECTORY BRK*` and `TYPE BRK*` differ only by BRK1RT.TXT, which OVMX's COPY 3c created before the
  loopback ran.

### Findings (not fixed here)

| finding | OVMX | VAX V7.3 |
|---|---|---|
| `COPY/LOG` of a remote wildcard: `%COPY-S-COPIED` line ending | `… copied to SYS$SYSROOT:[SYSMGR]BRK1.TXT;2` | `… copied to …BRK1.TXT;1 (1 block)` |
| $STATUS, `DIRECTORY …NOSUCH.TXT` (NOFILES) | %X10018290 | %X10018290: **now the same** (it was %X10018292 at b59e80288) |
| $STATUS, `TYPE VAXC"DNTEST pw"::SYS$LOGIN:BRK*.TXT` (BRKP OPENIN/PRV) | %X1001829A | %X10951098 |
| $STATUS, `TYPE VAXC"DNTEST pw"::SYS$LOGIN:BRKP.TXT` (OPENIN/PRV) | %X1001829A | %X10951098 |
| $STATUS, `DIRECTORY VAXC"DNTEST wrong"::SYS$LOGIN:BRK*.TXT` | %X1000209C | %X1001C02A |
| $STATUS, `TYPE VAXC"DNTEST wrong"::SYS$LOGIN:BRK1.TXT` | %X1000209C | %X10951238 |
| $STATUS, `DIRECTORY/FULL VAXC"DNTEST pw"::…` | %X00000E4A (refused) | %X00000001 |

## Files

`ovmxa-console.log`, `vaxc-console.log`, `events.log`, `wire.txt` (run 3; passwords XX), `oracle-diff.txt`, and
`bench/` (`fal_verbs.py`, `oracle_diff.py`, `decode_nsp.py`, `node.py`, `udp2mcast.py`, `vaxc.ini`).
