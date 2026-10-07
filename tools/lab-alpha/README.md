# tools/lab-alpha -- drive a disposable OpenVMS Alpha V8.4 lab from a script

Used to capture the oracle under `docs/oracle/alpha84-*` and to run real-service probes
(the lab has `MACRO` and `LINK`, so a MACRO-32 program can call the real service and
print what it returns). Observation only; nothing is disassembled (Rule 8).

Pod: copy `tests/lab-alpha/k8s/20-alphalab.yaml`'s container into a one-off Pod
(memory request == limit, a small CPU request if k3s-worker is full; see
`pod.example.yaml`). Set `POD` (default `corpusalpha-1`) and `NODE` (default `alpha1`).
Log in once by hand (`SYSTEM`, then the new password if it has expired).

| script | what |
|---|---|
| `asend.sh 'DCL line'` | base64-safe line to the console FIFO |
| `aq.sh 'DCL line' [secs]` | send a line, print the console output after it |
| `apush.py LOCAL REMOTE` | write a text file on the guest via `OPEN/WRITE` |
| `arun.sh NAME` | `MACRO` + `LINK` + `RUN SYS$SCRATCH:NAME` (push `NAME.MAR` first) |
| `extract_defs.py POD FIFO LOG LIB OUTDIR MOD...` | `LIBRARY/MACRO/EXTRACT=$MOD` from `SYS$LIBRARY:LIB.MLB`, then `slice_defs.py` |
| `slice_defs.py POD LOG OUTDIR MOD...` | cut the `$EQU` lines per module out of the console log (bytes-safe) |
| `probes/*.mar` | the MACRO-32 probes whose output is recorded in `docs/oracle/alpha84-probes/` |

Never `pkill -f` with a pattern that appears in your own command line.
