# vms-29e: what a real VMS console does with a login read

**2026-09-25. Real OpenVMS VAX V7.3 (`vax1`) on an isolated `vaxlab` replica
(`sts/vaxlab` scaled 5 to 6, `vaxlab-5`, then scaled back). FIFO console drive only.
No other lab node and no cluster configuration was touched.** The `runN.sh`
scripts are the exact drivers. The `.console.log` files are the raw console
bytes (CR/LF as SIMH logged them).

## The question

rd vms-29e: on a clustered OVMX node, SYSTEM typed at `Username:` brought back the
banner and a fresh `Username:` with no `Password:` prompt. The item blamed the
executive's operator lines (`%DLM`, `%CNXMAN`) for breaking LOGINOUT's read.

## What VMS does

**1. Both login reads time out after LGI_RETRY_TMO, and the timeout is announced**
(`run3-lgi-parameter-sweep.console.log`):

| SYSGEN | Username: read expired after | Password: read expired after |
|---|---|---|
| defaults: LGI_PWD_TMO 30, LGI_RETRY_TMO 20 | 19 s | 21 s |
| LGI_RETRY_TMO 60 | 59 s | 61 s |
| LGI_PWD_TMO 60, LGI_RETRY_TMO 20 | 20 s | 19 s |

The bytes printed each time:

```
\rUsername: \r\nError reading command input\r\nTimeout period expired\r\n\x07%%%%%%%%%%%  OPCOM ...
\rPassword: \r\nError reading command input\r\nTimeout period expired\r\n\x07%%%%%%%%%%%  OPCOM ...
```

Each timeout is followed by an AUDIT$SERVER alarm (`%LOGIN-F-CMDINPUT`). That
is the same signature as `tests/lab/captures/vms-e18e-cn3-browser-20260925/`,
where it appears on VAXC's console.

**2. After a timeout, the next typed line only wakes the terminal**
(`run1-timeout-and-wake.console.log`). `TEM<CR>` typed at a timed-out console
brought up a new banner and an empty `Username: `. The typed text was not used
as a username.

**3. A broadcast does not end a login read** (`run2-broadcast-during-read.console.log`):

```
\rUsername: SYS\r\n\r\nReply received on VAX1 from user SYSTEM at VAX1 Batch   10:19:34\r\nBCAST3\r\n\r\n
\rUsername: SYS\r\n\x07%%%%%%%%%%%  OPCOM  25-SEP-2026 10:19:34.49  %%%%%%%%%%%\r\n...OPCOMMSG3\n\r\n
\rUsername: SYSTEM\r\n\rPassword: \r\n\r\nReply received ...\r\nBCAST4\r\n\r\n Welcome to OpenVMS ...
```

A REPLY and an OPCOM message arrived while `SYS` was half typed. VMS showed each
message, printed the prompt and the typed characters again, and kept reading, so
`TEM` completed `SYSTEM`. A REPLY that arrived during the echo-off `Password:`
read did not end that read either, and the login succeeded. VMS did not print
the `Password:` prompt again.

## Reading vms-29e against this

In `vms-e18e-cn3-browser-20260925/cn3-pass-CAB/OVMXA.console.log`, OVMXA printed
`Username:` before uptime 340 s, which is about 08:43:27. The grader first typed
SYSTEM at about 08:48:55, 5.5 minutes later. By then LOGINOUT had timed out
silently: OVMX used 30 s and printed nothing. That SYSTEM<CR> only woke a new
session, which is also what VMS does (point 2). The operator lines printed in
between were a coincidence. OVMX's read is not broken by console output; the
guard in `tests/tools/test_loginout_display.c` (d) checks this.

The fidelity gaps that caused it are fixed in `tools/login_input.h`:

- The deadline is now 20 s (the LGI_RETRY_TMO default), down from 30 s.
- The two VMS lines are printed when the deadline expires.
- A half-typed line is discarded on expiry.

What is still missing is the reprint of the prompt and typed characters after a
broadcast (point 3). OVMX's `$BRKTHRU` is an unmediated terminal write
(sys_operator.c, OVMX-LOCAL), so no terminal driver can reprint a read's prompt
after it. That is tracked as rd vms-42f.
