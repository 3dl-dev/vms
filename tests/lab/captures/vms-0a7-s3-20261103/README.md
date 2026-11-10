# vms-0a7 S3 against a real VAX V7.3 (2026-11-03): NETACP.EXE, FAL through SYS$NET, the S1 client

rd vms-0a7 slice S3, branch `work/vms-0a7-s3-netacp`. These runs boot OVMXA from the branch. In
it, STARTNET runs `NCP SET EXECUTOR STATE ON` and NCP `$CREPRC`s `NETACP.EXE` detached, with its
output going to `SYS$MANAGER:NETACP.LOG`. NETACP serves inbound object 17 by `$CREPRC`ing
`FAL.EXE`, which takes its link through `SYS$NET`. The peer is a real OpenVMS VAX V7.3 (VAXC).

**Current result (branch head 771a65216, as shipped): S3 is still not ready.** In 771a65216
NETACP keeps NETACP.LOG open (one `$CREATE` with SHRGET, then `$PUT` + `$FLUSH` per line). That
did not clear the stalls:
- the inbound SET HOST login stalls after "Last interactive login";
- the 2000-record COPY fails or stalls in both directions;
- once, the OVMX **console** login at boot stalled the same way.

In every stalled run NETACP.LOG gets no line after the first process that NETACP creates. The run
is in `run-771a65216-shipped/` and summarised in the first table below.

Earlier runs are kept as history:

| fix | commit | result |
|---|---|---|
| A process leaked one POSIX timer for every expired `$SETIMR`, so NETACP stopped after a few thousand waits. | 40e8c4f7e | fixed |
| NETACP did one `$OPEN`/`$PUT`/`$CLOSE` per NETACP.LOG line. | 771a65216 | stalls remain |

**Run B, the diagnostic variant.** This is image 40e8c4f7e with NETACP restarted with
`/OUTPUT=NL:` (no NETACP.LOG). In run B every case passes against the VAX, byte-identical both
ways. It is not the shipped configuration.

## Shipped run on 771a65216 (`run-771a65216-shipped/`)

**Image.** `ovmx-boot-s3:bench-771a65216`, docker id `sha256:8f35c21ea61e…`, built by the branch
owner from 771a65216. Its `artifacts.sha256` gives `ovmx-distrib.img` as `90c9f651…`.

**Bench.** This is `bench/s3_bench.py` as shipped, with no NL: override. `S3_SETHOST_LAST=1` runs
the inbound SET HOST last. With the login first, the bench never got past it. Two more attempts are
kept:
- `login-first/`: the inbound login at the start. It stalls, and the OVMX console then shows
  SHOW SYSTEM and NETACP.LOG.
- `console-login-stall/`: the OVMXA console login itself stalled after "Welcome", so the bench
  never started.

| # | case | result on 771a65216 as shipped |
|---|---|---|
| 1a | NETACP is its own detached process (OVMX SHOW SYSTEM). | **PASS.** 10000004 NETACP. |
| 1b | NETACP.LOG has the DATALINK and STARTED lines. | **PASS** (2 lines). |
| 1c | VAXC `SET HOST OVMXA`, login, SHOW SYSTEM. | **FAIL** (`login-first/`). The banner and "Last interactive login" arrive, then no `$` for 120 s, and CTRL/Y CTRL/Y Y is needed on the VAX. During the stall OVMX shows NETACP alive with a nameless session process 10000007. NETACP.LOG ends at `SESSTART … RTA0:`: no RTATERM line, and no 15 s ADJINIT lines after it. In the main run the login was not reached, because 3e hung the console. |
| 2 | VAXC `DIRECTORY OVMXA"…"::` | **PASS** (2a, 2b). |
| 2 | VAXC `COPY BIG.TXT` into OVMX's FAL, back, DIFFERENCES | **FAIL.** `%COPY-E-WRITEERR … -SYSTEM-F-LINKDISCON` at 106 blocks. FAL `SERVER_0003` is still running afterwards and holding BIGV.TXT: OVMX's own COPY of it (2h) gets `-RMS-F-SHR`. The copy back and DIFFERENCES fail with FNF. |
| 3 | OVMX `DIRECTORY` / `TYPE VAXC"…"::`, wrong password | **PASS** (3a–3d). The wrong password gives `-SYSTEM-F-INVLOGIN`. |
| 3 | OVMX `COPY VAXC"…"::BIG.TXT` | **FAIL.** No prompt in 420 s, and the console does not return after CTRL/Y. |
| 4 | DECNETD.EXE never activated | **PASS.** It is deleted at login. `SET HOST VAXC` gives `%SET-I-NOTAVAIL`. No DECNETD process appears in any SHOW SYSTEM. |
| — | VAX console | No bugcheck. |

**Reading.** Keeping the file open did not help, so the per-line `$OPEN` was not the whole cause.
The stall signature is the same as with the old sink. What separates the passing runs is NETACP's
SYS$OUTPUT: with NETACP.LOG (through RMS) things stall; with `NL:` (run B), or as a foreground
image on a terminal, they do not.
- **Hypothesis.** NETACP's RMS access to NETACP.LOG stops working, or blocks, once NETACP has
  `$CREPRC`ed a process (LOGINOUT for CTERM, FAL for object 17). It contends with that process's
  file writes in `[SYSMGR]` (LASTLOGIN_SYSTEM.DAT, BIGV.TXT, the console COPY's BIG.TXT), and
  NETACP's single loop stalls.
- **The console-login stall.** At boot, NETACP's open NETACP.LOG and the console LOGINOUT's write
  in the same directory deadlock, which points at the ACP/RMS file-access layer.
- **Not proven.** The exact lock.

## History: runs on 3c4df7549 and 40e8c4f7e

## Images and artifacts

| image (local tag, removed after) | git SHA | docker image id |
|---|---|---|
| `ovmx-boot-s3:bench-3c4df7549` (as handed over) | 3c4df7549 | `sha256:da5623ed1318…` |
| `ovmx-boot-s3:s3r1-40e8c4f7e` (with the two fixes below) | 40e8c4f7e | `sha256:1c4a913d8fa0…` |
| `ovmx-boot-s3:bench-771a65216` (NETACP.LOG kept open) | 771a65216 | `sha256:8f35c21ea61e…` |

Both were built with `docker build -f distro/Dockerfile.bootable --target runner`. The sha256 of
the kernel, initramfs, distribution disk and DECnet-injected OVMXA disk are in each run's
`artifacts.sha256`.

VAXC is the Node C DECnet disk (`974af57e…`, a fresh scratch copy for every run), licensed from the
lab PAKs. The console log is paused while the PAKs are typed, so they are redacted entirely.

## Bench (`bench/`)

The bench is the S1 run-5 bench with its own names and ports: container `s3r1-vaxc`, SIMH udp
41095/41096, mcast 230.0.0.95:12495, work dir `/tmp/s3r1-lab`. Passwords come from the
environment.

**Prerequisites.** These were taken from the S2 helper's `/tmp/vms0a7s2-lab`, which got them from
the lab PVC: `nodec.dsk.gz` (`/lab/cluster-demo-nodeC-v73-decnet/`), `secret/paks3.json` (from
`/lab/cluster/PAKS.COM`) and an empty `nvram.bin`.

| file | what it does |
|---|---|
| `run.sh` | Extracts the boot set from the image, injects DECnet and runs the bench. |
| `s3_bench.py` | The cases below. `NETACP_NL=1` gives the run B variant. |
| `probe_sethost.py` | The diagnostic probes. |

**Bench fixes.**
- The VAX login waits for a quiet `Username:`. OPCOM's audit text "Username:   SYSTEM" had matched.
- OVMXA's prompt is matched as `CR LF CR NUL "$ "`. NETACP.LOG text contains "$ SET HOST".
- A step that hangs is ended with CTRL/Y and recorded as FAIL.

## Results on 40e8c4f7e (history)

Each cell gives the run and the step tag from that run's `events.log`. The $STATUS values are
also in `events.log`.

- **A** = `run-40e8c4f7e-shipped-copy-login/` and `run-40e8c4f7e-shipped-bench/`: the fixed
  image as shipped.
- **B** = `run-40e8c4f7e-netacp-nl/`: the fixed image with NETACP's output on `NL:`.

| # | case | as shipped (A) | variant (B) |
|---|---|---|---|
| 1a | NETACP runs as its own detached process. OVMX `SHOW SYSTEM` lists `NETACP`. | **PASS.** 10000004 NETACP, started by NCP from STARTNET. | **PASS.** 10000007, the restarted one. |
| 1b | `TYPE SYS$MANAGER:NETACP.LOG` shows the DATALINK and STARTED lines. | **PASS.** Both lines are there, with `datalink open on ETH0: via the executive`. | — (no log in B) |
| 1c | VAXC `SET HOST OVMXA`, login, `SHOW SYSTEM` lists NETACP. | **FAIL.** The login stalls after "Last interactive login": no `$` in 60 s (`run-40e8c4f7e-shipped-copy-login`, step P5). NETACP is alive, but NETACP.LOG gets no line after the first server process. | **PASS, twice** (1e and 9e). VAXC lists `NETACP` and `_RTA0:` on node OVMX, and `F$GETSYI("NODENAME")` works. |
| 2 | VAXC `DIRECTORY OVMXA"SYSTEM pw"::` | **PASS.** 15 files of `SYS$SYSROOT:[SYSMGR]`. | **PASS** (2a). |
| 2 | VAXC `COPY BIG.TXT` (2000 records, 290 blocks) into OVMX's FAL, then back, then DIFFERENCES | **FAIL.** `%COPY-E-WRITEERR … -SYSTEM-F-LINKDISCON` after 41 blocks. The FAL process (`SERVER_0002`) is left running. | **PASS.** In 43 s; BIGV 290 blocks; back as BIGVB. DIFFERENCES finds 0 sections. CHECKSUM 33952600 = 33952600. |
| 2 | NETACP `$CREPRC`s FAL.EXE with SYS$NET | **PASS.** NETACP.LOG: `FALSTART … FAL.EXE pid 10000008 created as UIC [1,4]; the link waits for it to accept (SYS$NET)` then `FALACCEPT … Connect Confirm sent`. | **PASS.** `SERVER_0003` appears in OVMX's SHOW SYSTEM samples during the COPY. |
| 2h | OVMX sends the FAL-received BIGV.TXT back to VAXC as BIGVRT | — | **PASS.** DIFFERENCES 0. CHECKSUM 33952600. |
| 3 | OVMX `DIRECTORY` / `TYPE VAXC"…"::`, and a wrong password | **PASS** (3a–3d). The wrong password gives `-SYSTEM-F-INVLOGIN`, %X1001C02A. | **PASS.** |
| 3 | OVMX `COPY VAXC"…"::BIG.TXT`, back as BIGRT, then DIFFERENCES | **FAIL.** The link runs about 10 segments every ~3 s, stops at ~44 s, and OVMX sends DI (reason 0) at 104 s. The console COPY never returns. | **PASS.** In 67 s, back in 11 s. DIFFERENCES 0. CHECKSUM 33952600 = 33952600. |
| 4 | DECNETD.EXE never activated | **PASS.** `DELETE SYS$SYSTEM:DECNETD.EXE;*` is run on the scratch disk right after login, and DIRECTORY then finds none. The positive control `SET HOST VAXC` (still the DECNETD client until S2) gives `%SET-I-NOTAVAIL`. All other cases ran without the image. Every SHOW SYSTEM sample has no DECNETD process. FAL appears as `SERVER_nnnn`. | **PASS**, the same way. |
| — | VAX console | No bugcheck in any run. A grep for bugcheck, fatal or crash in every `vaxc-console.log` finds 0. | same |

## The fixes in this branch (local commits, not pushed)

**36845b3c7 — the image injector `DEFINE`s the executor.**
- **The problem.** `inject-decnet-config.sh` runs NCP offline against a disk image. Since S3,
  `SET EXECUTOR STATE ON` `$CREPRC`s NETACP, so the injector failed with
  `%NCP-F-OPEFAI -SYSTEM-F-NOPRIV`. The first run of this bench showed it.
- **The fix.** `mk_democonfig.py` now emits `DEFINE EXECUTOR ADDRESS/NAME/STATE ON`, the permanent
  database a site writes before a boot. `test_mk_democonfig.py` is updated (33 pass).

**40e8c4f7e — an expired `$SETIMR` gives back its POSIX timer** (`src/libvms/syssvc/sys_time.c`).
- **The leak.** `timer_signal_handler` marked the request done but never deleted the timer. Each
  timer holds one RLIMIT_SIGPENDING entry, so every expired `$SETIMR` leaked one.
- **Why NETACP hit it.** NETACP's `$WFLOR` loop sets a timer for every wait: 5 ms while a link is
  live, 100 ms when idle.
- **Proof on the old image.** `diag-3c4df7549/probe6`, NETACP in the foreground, prints
  `DECNETD-F-SETIMR, $SETIMR failed (status 00000124); NETACP cannot keep its timers and stops`
  during a VAX COPY.
- **What it looked like.** Detached, NETACP just disappeared from SHOW SYSTEM (`runA`, `probe1`):
  in the middle of the BIG COPY and at the inbound login.
- **The fix.** The slot keeps its timer until the slot is next claimed or cancelled, and deletes it
  there, outside signal context.
- **The test.** `test_libvms_setimr_timer_reuse` sets RLIMIT_SIGPENDING to 64 and runs 256 expiring
  requests, then checks that `$CANTIM` still cancels a pending one. Before the fix it fails at #65
  with 00000124. Host ctest is 423/423 (e2e env-skipped, as baseline).
- **After the fix.** NETACP stays up: in the fixed image's first run (`run-40e8c4f7e-shipped-bench`)
  hellos keep going out for 6+ min.

## The NETACP.LOG failure as diagnosed on 40e8c4f7e (771a65216 then changed the sink; see the shipped run above)

**The bisect.** One image (40e8c4f7e), one VAX, one bench, and only NETACP's SYS$OUTPUT differs:

| NETACP's output | what happens |
|---|---|
| `NETACP.LOG` (as shipped) | The BIG COPY stalls both ways and the inbound login stalls. |
| `NL:` | Everything passes. |

The foreground `RUN NETACP.EXE` (output on the terminal, `probe3`) also completes an inbound login,
SHOW SYSTEM and LOGOUT.

**What the logs show.**
- NETACP.LOG gets no line once another process is writing a file in `SYS$SYSROOT:[SYSMGR]`. That
  directory holds NETACP.LOG and is SYSTEM's `SYS$LOGIN`.
- The writers are FAL writing `BIGV.TXT`, the console COPY writing `BIG.TXT`, and LOGINOUT writing
  `LASTLOGIN_SYSTEM.DAT`. In `run-40e8c4f7e-shipped-copy-login` the last line is the FALACCEPT at
  04:54:23. The 15 s ADJINIT lines stop there too, while NETACP keeps running.

**Hypothesis.**
- **The mechanism.** `rms_textfile_append_line` (`src/libvms/rtl/rms_textfile.c`) opens through
  `rmstf_open_wait`. That retries RMS$_SHR 100 × 10 ms and then drops the line, so while the file
  or directory is busy every NETACP log line blocks NETACP's single loop for at least 1 s.
- **What that explains.** The outbound flow-control resume comes about 3 s late, the link stalls,
  and the login session hangs.
- **Not yet proven.** Which ACP lock or holder it is.
- **A likely fix.** Keep NETACP.LOG open for NETACP's life (one `$OPEN`, then `$PUT` per line), as
  a VMS process keeps its SYS$OUTPUT open, instead of `$OPEN`/`$CLOSE` per line. The other option
  is never to block the network loop on the log.

## Files

| path | contents |
|---|---|
| `run-771a65216-shipped/` | The current shipped run (case table above), plus `login-first/` and `console-login-stall/`. Consoles, `events.log`, `wire.txt.gz`, `artifacts.sha256`. |
| `run-40e8c4f7e-netacp-nl/` | Run B, every case PASS. Consoles, `events.log`, `wire.txt.gz` (NSP decode, `SHOWN=1`, passwords XX), `artifacts.sha256`. |
| `run-40e8c4f7e-shipped-copy-login/` | The fixed image as shipped: VAX→OVMX FAL BIG COPY fails at 41 blocks, then the inbound login stalls. OVMX SHOW SYSTEM and NETACP.LOG are taken during the stall. |
| `run-40e8c4f7e-shipped-bench/` | The fixed image as shipped: the OVMX→VAX BIG COPY stalls (the wire shows the 3 s flow-control cadence and the DI at 104 s). |
| `diag-3c4df7549/` | The handed-over image. See below. |

`diag-3c4df7549/` contains:
- `runA`: NETACP disappears during the BIG COPY. The wire shows OVMX sending DI reason 0 to all
  links at NETACP's shutdown.
- `probe1`: NETACP disappears at the inbound login. The kernel is at loglevel 7 with
  print-fatal-signals, and prints no fatal signal.
- `probe3`: foreground NETACP; the inbound login works.
- `probe6`: foreground NETACP; the `DECNETD-F-SETIMR … 00000124` proof.

**Passwords** are XX in every committed file. Neither pcap is committed. A grep of this directory
for the lab passwords finds 0.
