# INSTALL /PRIVILEGED observed on real VMS (rd vms-7c64 / vms-5ce5)

Captured 2026-10-09 on the ovmx-lab nodes (OpenVMS VAX V7.3, pod n3b3f-vax;
OpenVMS Alpha V8.4, pod n3b3f-alpha) as SYSTEM, with our own probe
`PRIVPROBE.MAR`. It prints `$GETJPI` CURPRIV / IMAGPRIV / PROCPRIV (high
longword first) and the status of a `$CMKRNL` call. Observed output only;
nothing was disassembled.

The command sequence and its verbatim output are in `<ARCH>-INSTALL-PRIV.txt`.
`VAX-INSTALL-PRIV-TRACEBACK.txt` is the first VAX run, which linked with traceback.

What the captures show:

- An image linked with traceback cannot be installed /PRIVILEGED:
  `%INSTALL-E-FAIL` with `-INSTALL-E-IMGTRACED` (VAX).
- With process privileges set NOALL, an image installed `/PRIVILEGED=CMKRNL`
  runs with CURPRIV = IMAGPRIV = CMKRNL and PROCPRIV = 0, and `$CMKRNL`
  succeeds. After the image exits, SHOW PROCESS/PRIVILEGES lists no process
  privileges. Once the image is removed, `$CMKRNL` is SS$_NOPRIV (%X24) (VAX).
- INSTALL LIST and LIST/FULL formats:
  - VAX: `Prv`, `Entry access count`, `Privileges = ...`.
  - Alpha V8.4: also `Open` (implied), `Current / Maximum shared`, and an
    `Authorized = ...` line.
- On Alpha, `SET PROCESS/PRIVILEGES=ALL` did not restore CMKRNL after NOALL, so
  REPLACE and REMOVE were refused there with `%SYSTEM-F-NOCMKRNL`. That is
  itself an observation; the entry stayed installed.
