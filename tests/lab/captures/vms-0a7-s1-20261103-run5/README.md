# vms-0a7 S1, run 5: DIRECTORY, TYPE and COPY node:: against a real VAX V7.3 (2026-11-03)

rd vms-0a7 S1, rd vms-36aa (RAB$L_RHB), rd vms-5b9 (DIRECTORY/FULL dates), rd vms-985 (NSP flow
control). Branch `work/vms-0a7-s1-rms-remote`, PR #1994 (draft). This repeats run 4
(`../vms-0a7-s1-20261102-run4/`) on the same bench against the same real VAX. It fixes run 4's two
mismatches.

**Result: every case matches the VAX.** Here is what that covers:
- the same-run VAX loopback (B) is IDENTICAL byte for byte for every command, blank lines included;
- every `$STATUS` equals the VAX's own L-line;
- every round trip is byte-identical (DIFFERENCES 0, CHECKSUM equal), BIG and LONG both ways included;
- DECNETD.EXE is never activated by DIRECTORY, TYPE or COPY.

## Runs

| run | image (git SHA) | what happened |
|---|---|---|
| 5a | e959f0c4b (RAB$L_RHB, A-18 dates) | COPY/LOG of BIG.TXT printed `(290 blocks)`, which matches the VAX; BIG.TXT and BIGV.TXT were 290 blocks on OVMX. The only remaining difference was DIRECTORY/FULL: `Created: 3-NOV-2026 00:29:11.00` where the VAX printed `.25`. The wire showed why: the VAX FAL sent OVMX its dates as A-18 text (DATE-TIME menu `0b`, `03-NOV-26 00:29:11`), which has no hundredths. Logs are in `run5a-e959f0c4b/`. |
| probe | e959f0c4b + `syscap-probe/experiment-hook-on-e959f0c4b.diff` (never committed to the branch) | Black-box probe of the VAX FAL, in `syscap-probe/` (see below). It found that SYSCAP bit 65 asks for the UTC dates. |
| **5b** | **cb10f5731** | **These logs.** Every step completed. Bench exit 0, `DONE` at 01:10:17. |

**Image for 5b:**
- Built with `docker build -f distro/Dockerfile.bootable --target runner` from the worktree at cb10f5731.
- DECnet 1.963 was injected with `tools/cluster-web-demo/inject-decnet-config.sh --scsnode OVMXA` (PASS).
- The sha256 of the kernel, initramfs and disk are in `artifacts.sha256`. The image was then removed.

## Bench (`bench/`)

This is run 4's bench with five changes:
- **Own names and ports.** Container `vms0a7s1r5-vaxc`, SIMH udp 41091/41092, mcast 230.0.0.91:12491.
- **Passwords come from the environment.** They are `VAX_PW`, `DN_PW`, `OVMX_PW` and `BAD_PW`.
- **`L1f0`.** The VAX runs DIRECTORY/FULL right after OVMX's 1f, so both list the same directory.
- **Loopback order.** The COPY/LOG loopbacks L3d and L3i run first. Then the bench deletes BRK1RT.TXT, which OVMX's 3c made after OVMX's 1d, 1f and 2c had run. Only then do L1d, L1f and L2c run. Both sides therefore list the same files. In run 4 the loopback showed one extra file.
- **Two new steps.** `3j` and `7e` are local `DIRECTORY/FULL` listings of the copied VFC files on OVMX.

The rest is unchanged:
- **VAXC:** a scratch copy of the Node C DECnet disk (gz sha256 `974af57e…`), licensed from the lab PAKs. The console log is paused while the PAKs are typed, so they are redacted entirely.
- **Wire:** frames from OVMXA's NIC are captured by qemu `filter-dump`.
- **Running it:** `bench/run.sh <image> <outdir>` extracts the boot set from the image, injects DECnet and runs the bench.

**Passwords:** `XX` in every committed file. The pcap is not committed. `wire.txt.gz` is its decode,
with passwords masked. A grep for the lab passwords in this directory finds 0.

## Results (run 5b; $STATUS from `events.log`; VAX side = its own L-step or its DIFFERENCES/CHECKSUM)

| # | case (on OVMXA unless VAXC) | OVMX result | VAX result | $STATUS OVMX / VAX | verdict |
|---|---|---|---|---|---|
| 0a–0b | `DEFINE/PROCESS SYS$SYSTEM SYS$LOGIN:`, then `SET HOST VAXC` (positive control) | DIRECTORY SYS$SYSTEM:DECNETD.EXE finds no file; SET HOST → `%SET-I-NOTAVAIL` | — | %X00000001 / — | control valid |
| 0c–0f | DIRECTORY / TYPE / COPY BRK2 both ways with DECNETD.EXE unreachable | all work | BRK2RT: DIFFERENCES 0, CHECKSUM 1225656911 = 1225656911 | %X00000001 / %X006C8009 | **PASS**: no DECNETD.EXE activation |
| 0g | `COPY VAXC::BIG.TXT` with DECNETD.EXE unreachable | completes in 37 s, no DI | — | %X00000001 / — | **PASS** |
| 6a samples | VAXC `SET HOST OVMXA` samples `SHOW SYSTEM` 3× during the LONG COPY | OPCOM, NETACP, STARTUP, SYSTEM, JOB_CONTROL and _RTA0: in every sample; no DECNETD or FAL process | — | — | **PASS** |
| 1a–1c | DIRECTORY `*.COM`, `NOSUCH*`, `BRK*.TXT` (SYSTEM) | lists; `%DIRECT-W-NOFILES` | — | %X00000001, %X10018290, %X00000001 | PASS |
| 1d | `DIRECTORY VAXC"DNTEST pw"::SYS$LOGIN:BRK*.TXT` | identical to L1d | same | %X00000001 / %X00000001 | **PASS** (run 4: one more file on the VAX) |
| 1e | `DIRECTORY …NOSUCH.TXT` | identical to L1e | same | %X10018290 / %X10018290 | **PASS** |
| 1f | `DIRECTORY/FULL VAXC"DNTEST pw"::SYS$LOGIN:BRK*.TXT` | identical to L1f0 and L1f, including `Created:  3-NOV-2026 00:54:24.90` / `Revised: … (2)` | same | %X00000001 / %X00000001 | **PASS** (run 4: `<None specified>`) |
| 2a–2b | `TYPE` one file / a wildcard (SYSTEM) | the records, with per-file headers | — | %X00000001 | PASS |
| 2c | `TYPE VAXC"DNTEST pw"::…BRK*.TXT` | identical to L2c (BRKP → OPENIN/-RMS-E-PRV) | same | %X10951098 / %X10951098 | **PASS** |
| 2d | `TYPE …BRKP.TXT` | identical to L2d | same | %X10951098 / %X10951098 | **PASS** |
| 5a / 5b | wrong password: DIRECTORY / TYPE | identical to L5a / L5b; the password is not echoed | same | %X1001C02A / %X1001C02A, %X10951238 / %X10951238 | **PASS** |
| 3a–3c | COPY BRK1 VAX→OVMX→VAX as BRK1RT | completes | DIFFERENCES 0; CHECKSUM 742093723 = 742093723 | %X00000001 / %X006C8009 | **PASS** |
| 3d | `COPY/LOG VAXC"SYSTEM pw"::…BRK*.TXT SYS$LOGIN:*.*` | identical to L3d | same | %X00000001 / %X00000001 | **PASS** |
| 3f / 3g / 3h | COPY BIG.TXT VAX→OVMX (46 s); `DIRECTORY/SIZE` there; OVMX→VAX as BIGRT (9 s) | **BIG.TXT 290 blocks** (run 4: 297) | BIG 290/297 and BIGRT 290/297; DIFFERENCES 0; CHECKSUM 33952600 = 33952600 | %X00000001 / %X006C8009 | **PASS**, byte-identical |
| 3i | `COPY/LOG VAXC::BIG.TXT SYS$LOGIN:BIGL.TXT` | `… (290 blocks)`; identical to L3i | `… (290 blocks)` | %X00000001 / %X00000001 | **PASS** (run 4: 297) |
| 3j | OVMX `DIRECTORY/FULL SYS$LOGIN:BIGL.TXT` (local) | `Record format: VFC, 2 byte header, maximum 0 bytes, longest 69 bytes`, the VAX's line for BIG.TXT | — | %X00000001 | record format matches. The local /FULL layout is not the VAX's (see below) |
| 6a / 6b | COPY LONG.TXT (20000 rec) VAX→OVMX (9 min 10 s), then OVMX→VAX as LONGRT (1 min 30 s) | both complete | LONG/LONGRT 3125/3132; DIFFERENCES 0; CHECKSUM 2 = 2 | %X00000001 / %X006C8009 | **PASS**, byte-identical |
| 4a–4g | VAXC COPY V2O.TXT into OVMX's FAL and back; DIRECTORY of OVMXA | complete | DIFFERENCES 0; CHECKSUM 2033922319 = 2033922319 | — / %X00000001 | PASS |
| 7a / 7b | VAXC `COPY BIG.TXT OVMXA"SYSTEM pw"::BIGV.TXT` (into OVMX's FAL, 55 s); OVMX `DIRECTORY/SIZE` | **BIGV.TXT 290 blocks** (run 4: 297) | — | — / %X00000001 | **PASS** |
| 7c / 7d | OVMX `COPY SYS$LOGIN:BIGV.TXT VAXC::BIGVRT.TXT` (10 s) | completes | BIGVRT 290/297; DIFFERENCES 0; CHECKSUM 33952600 = 33952600 | %X00000001 / %X006C8009 | **PASS** |

`oracle-diff.txt` (A) compares against the 2026-10-08 VAX-to-VAX capture. Its fixture files were VFC
and its directory had no BRK2RT, and its dates are of that run. Those are fixture differences, not OVMX
differences. (B) is the same-run loopback, which is IDENTICAL for every command.

## What changed since run 4

**rd vms-36aa** (commit e959f0c4b): OVMX RMS now implements RAB$L_RHB, per the RMS Reference.
- **The record path.** A VFC record's fixed control goes to and from the record header buffer, in local `$GET`/`$PUT`, remote `$GET`/`$PUT`, COPY, the FAL's store and send, and the VMS-ABI veneer. `$OPEN` loads FAB$B_FSZ.
- **On disk.** The 2000 records of BIG.TXT are 74 bytes each: count 71, control `01 8D`, 69 bytes of text and a pad byte. That is 148000 bytes, so EOF is in block 290, as on the VAX.
- **The old cause.** Run 4 wrote 4 more bytes per record (zeroed control + the control in the data), which made 297 blocks.

**rd vms-5b9** (commits e959f0c4b, cb10f5731): the 64-bit dates are now asked for.
- **What the VAX sends by default.** The VAX FAL sends OVMX's accessor the A-18 text dates unless the accessor's CONFIGURATION asks for the 64-bit UTC form.
- **What asks for them.** The bit that asks is not in the public DAP 5.6 list. It was found black-box against this VAX (`syscap-probe/`): OVMX's accessor advertised, one at a time, each SYSCAP bit the VAX advertises and OVMX did not, and ran DIRECTORY/FULL each time.
- **Which bit.** Only bit 65 changed the reply: DATE-TIME `88 03` plus two 16-byte UTC times, as a VAX gets. Every other byte of the DIRECTORY LIST reply was the same.
- **One other bit.** Bit 66 alone changed the ATTRIBUTES layout, and DIRECTORY/FULL then printed no date. It is not advertised.
- **What OVMX does now.** OVMX's accessor sets bit 65 and decodes that form.
- **On the run 5b wire.** The run 5b wire carries 54 accessor CONFIGURATIONs with bit 65 (`… a0 ac 80 80 80 04`). The VAX's three DATE-TIME replies to case 1f are `0d 02 24 88 03 …`. The A-18 text is also parsed, for a FAL that sends only that.

## On the wire (`wire.txt.gz`)

- **Disconnects.** OVMX sent 53 DIs, all with reason 0. The VAX's two DIs with reason 34 are the two wrong-password cases. There is no DI with reason 1.
- **The two A-18 DATE-TIME messages on this wire** (`0d 02 27 0b …`) were sent by OVMX's FAL server, to VAXC's own accessor in case 4. That server's CONFIGURATION is unchanged.

## Not part of S1, filed

The local (not node::) `DIRECTORY/FULL` listing on OVMX (3j, 7e) does not have the VAX's layout:
- `Size: 290/290` where the VAX has used/allocated;
- `Expired:` where the VAX has `Expires:`;
- the Effective, Recording, organization and attribute lines are missing.

Filed as rd vms-f32. The remote listing that S1 covers (1f) is byte-identical.

## Files

- **Run 5b:**
  - logs: `ovmxa-console.log`, `vaxc-console.log`, `events.log`
  - comparisons and wire: `oracle-diff.txt`, `wire.txt.gz`
  - image hashes: `artifacts.sha256`
- `run5a-e959f0c4b/`: run 5a's consoles, events, oracle diff and hashes.
- `syscap-probe/`:
  - the probe: `syscap_probe.py`, which imports `fv.py` (= `bench/fal_verbs.py` with the container renamed `vms0a7s1r5x-vaxc`, mcast 230.0.0.93:12493, udp 41093/41094);
  - the experiment-only hook: `experiment-hook-on-e959f0c4b.diff`, which reads the extra bits from the process logical `S1R5_SYSCAP`;
  - its logs and `wire.txt.gz` (full DAP bytes).
- `bench/`: `fal_verbs.py`, `oracle_diff.py`, `decode_nsp.py` (`SHOWN=` prints whole segments), `node.py`, `udp2mcast.py`, `vaxc.ini`, `run.sh`.
