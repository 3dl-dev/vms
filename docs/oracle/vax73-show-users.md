# Oracle: `SHOW USERS` and `SHOW USERS/FULL` on OpenVMS VAX V7.3

**Item:** vms-050 (DCL/SHOW UX-fidelity sweep). **Node:** VAX1, OpenVMS VAX
V7.3, 2026-08-29. **Lab:** lab-2 (`tests/lab`), an isolated k3s replica
(`vaxlab-1`, scaled up for this capture and scaled back after). **Method:**
logged in as SYSTEM on the console, `SET TERMINAL/PAGE=0/WIDTH=132/NOBROADCAST`,
then each command run between `===TAG===` markers and the console log sliced and
read through `cat -A` so column positions are **counted from bytes, not
eyeballed**. Documented tool output only -- no disassembly, no VSI source
(CLAUDE.md Rule 8).

**Why measured.** `cmd_show_users()` in `src/vmsdcl/dcl_cmd_show.c` carried a
standing note that "No oracle capture exists for SHOW USERS (docs/oracle/ has
none)", so its column layout was grounded only in the *printed* DCL Dictionary
example (6-space indent). A live console is the more authoritative Rule 8 source;
this file supplies it and the code now matches the bytes below.

---

## 1. `SHOW USERS`, verbatim (`cat -A`, `^M$` = CRLF)

```
      OpenVMS User Processes at 29-AUG-2026 15:51:44.74
    Total number of users = 1,  number of processes = 1

 Username  Node     Interactive  Subprocess   Batch
 SYSTEM     VAX1            1
```

Column positions (1-based), measured:

- Header: 1 leading space; `Username` cols 2-9; `Node` cols 12-15;
  `Interactive` cols 21-31; `Subprocess` cols 34-43; `Batch` cols 47-51.
- Data row: 1 leading space; username left-justified in an 11-wide field
  (cols 2-12); node in a 6-wide field (cols 13-18); the Interactive **count**
  right-justifies with its right edge on **col 29** (an 11-wide value field,
  cols 19-29). Subprocess (12-wide) and Batch (8-wide) follow; a zero count
  renders **blank**, never a literal `0`.
- Summary line wording is verbatim: `    Total number of users = N,  number of
  processes = M` (two spaces after the comma).

## 2. `SHOW USERS/FULL`, verbatim (`cat -A`)

```
      OpenVMS User Processes at 29-AUG-2026 15:51:47.88
    Total number of users = 1,  number of processes = 1

 Username  Node   Process Name    PID     Terminal
 SYSTEM     VAX1  SYSTEM        2020021A  OPA0:
```

Column positions:

- Header: 1 leading space; `Username` 2-9; `Node` 12-15; `Process Name` 19-30;
  `PID` 35-37; `Terminal` 43-50. (Hand-spaced literal, distinct from the data
  field widths -- exactly as VMS emits.)
- Data row: 1 leading space; username `%-11s` (cols 2-12); node `%-6s`
  (13-18); process name `%-14s` (19-32); PID `%08X` (33-40); two spaces;
  terminal left-justified (43-).

## OVMX rendering note (INV-0 trademark ceiling)

OVMX prints `OVMX User Processes at ...` where VMS prints `OpenVMS User
Processes at ...` -- a deliberate rebrand (INV-0), not a fidelity defect. Every
other byte of the two forms matches the capture above.
