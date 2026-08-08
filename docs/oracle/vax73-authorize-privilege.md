# AUTHORIZE without SYSPRV — the exact refusal message (VAX V7.3)

Clean-room observation oracle (project Rule 8). Message text derived ONLY from
running the shipped VMS `AUTHORIZE` utility on the reference lab and reading what
it printed. No VSI/HPE source or binary was disassembled.

## What was being pinned

OVMX's `AUTHORIZE` (`tools/vms_authorize.c`) refused a process without SYSPRV
with an **invented** diagnostic:

```
%UAF-F-NOAUTH, insufficient privilege to manage SYSUAF
```

`%UAF-F-NOAUTH` is not a real AUTHORIZE message. This capture establishes what
VMS actually prints so OVMX can match it (`vms-4d7`).

## Capture

- **Lab:** lab-2 replica `vaxlab-7` (k3s `ovmx-lab`), OpenVMS **VAX V7.3**,
  2-node cluster VAX1/VAX2 sharing `SYS$COMMON` on the dual-ported disk.
- **Date:** 8-AUG-2026.
- **Method:** AUTHORIZE opens `SYS$SYSTEM:SYSUAF.DAT`, whose protection is
  `[SYSTEM]` `(RWE,RWE,,)` — owner UIC `[1,4]`, **no WORLD access**. A process
  that is (a) not the owner and (b) holds no SYSPRV/BYPASS/GRPPRV/READALL is
  refused by RMS at open time. AUTHORIZE does **not** pre-check a privilege bit;
  the refusal is RMS's.

A temporary unprivileged account was created for the probe and removed
afterward (the lab was restored to its prior state — SYSUAF record removed, the
`SYSUAF` logical deassigned):

```
UAF> ADD TESTNP /PASSWORD=... /UIC=[200,200] /PRIVILEGES=(TMPMBX,NETMBX) ...
```

Logged in as `TESTNP` on node VAX2 and confirmed the process identity:

```
$ SHOW PROCESS/PRIVILEGES
                          User: TESTNP           ... Process name: "TESTNP"
Authorized privileges:
 NETMBX    TMPMBX
Process privileges:
 NETMBX               may create network device
 TMPMBX               may create temporary mailbox
```

Then, with the `SYSUAF` logical pointed at the real file so the ONLY thing that
could stop the open is protection:

```
$ DEFINE SYSUAF SYS$COMMON:[SYSEXE]SYSUAF.DAT
$ MCR AUTHORIZE
%UAF-E-NAOFIL, unable to open system authorization file (SYSUAF.DAT)
-RMS-E-PRV, insufficient privilege or file protection violation
$
```

## The result

The exact two-line message a non-SYSPRV, non-owner process gets from AUTHORIZE:

```
%UAF-E-NAOFIL, unable to open system authorization file (SYSUAF.DAT)
-RMS-E-PRV, insufficient privilege or file protection violation
```

- `%UAF-E-NAOFIL` — AUTHORIZE's "unable to open the authorization file" error,
  severity **E** (error), text names the file `(SYSUAF.DAT)`.
- `-RMS-E-PRV` — the RMS sub-status naming the cause: *insufficient privilege or
  file protection violation*.
- AUTHORIZE then **exits** to DCL. It does NOT prompt "Do you want to create a
  new file?" — that prompt appears only for `-RMS-E-FNF` (file not found), which
  is a different failure (confirmed in the same session: the file's owner,
  SYSTEM, with SYSPRV dropped but still matching owner protection, got FNF and
  the create prompt, not PRV).

## Where this is used

`tools/vms_authorize.c` `check_privilege()` prints exactly these two lines when
the caller lacks SYSPRV. OVMX pre-checks the SYSPRV bit rather than letting RMS
refuse the open (a design choice — the executive can answer the privilege
question directly), but the **reported text matches VMS**.

Asserted in `tests/qemu/test_syssvc_authorize.c` (existing, message updated) and
`tests/qemu/test_syssvc_identcont.c` (new).
