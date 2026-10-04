# Real OpenVMS VAX V7.3: NCP and SHOW NETWORK output (oracle for NCP fidelity)

These transcripts come from the DECnet lane's own isolated lab pod `dnlab-1`, node VAX1 (1.1), on 2026-10-04. Each file is the console output of the command its name spells out. They are the layout ground truth that OVMX's `NCP.EXE` and DCL `SHOW NETWORK` are diffed against.

Clean-room note (Rule 8): this records observed output only.

Server-account passwords in `SHOW KNOWN OBJECTS` are replaced by same-length `x` placeholders (INV-0).

## What the layouts show

- Every `NCP SHOW` output opens with a "... as of <date>" header, then the `Executor node = a.n (NAME)` line.
- `SHOW KNOWN NODES` lists the executor block first, then a node table with these columns: `Node`, `State`, `Active Links`, `Delay`, `Circuit`, `Next node`.
- The counters and characteristics are live executor state. OVMX must read those values from its running NETACP/executive. It must never invent them (INV-6).
