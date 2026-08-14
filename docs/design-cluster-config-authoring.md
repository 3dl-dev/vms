# Design record — VMS-faithful cluster configuration (authoring)

> Epic backing document. Placement map + release ladder for closing the
> config-**authoring** gap in the clustering program. Source-of-truth for the
> `vms-cfgcluster` epic (see rd). Written 2026-08-13.

## The gap (measured)

Config **reading** is faithful and DONE (`vms-ci.8`, merged d76a0c2): `scsd`
resolves SCSNODE / SCSSYSTEMID / group+password from a typed SYSGEN store that
mirrors `VAXVMSSYS.PAR`, with hardcoded values reduced to honest fallbacks
(`src/vmsscs/scsd.c:206,237,257`).

Config **authoring** — the operator setting that config *the VMS way* — is
split across three states:

| Piece | State | Where |
|---|---|---|
| SYSGEN param **read** at boot | DONE | `vms-ci.8`; `scsd.c` |
| `.PAR` write mechanism, conversational SYSBOOT | in epic | `vms-46c` (boot-faithful) |
| SYSMAN PARAMETERS SET/SHOW (string params) | filed, numeric-only today | `vms-8da` |
| CLUSTER_AUTHORIZE hash `(group#,pw)→credential` | filed (RE) | `vms-732`; must-auth decision `vms-405` |
| VOTES/EXPECTED_VOTES wire reconciliation | filed (RE) | `vms-41d`; quorum-loss `vms-2d6` |
| **Real CLUSTER_AUTHORIZE.DAT on-disk format** | **UNSCOPED** — only a "MINIMAL OVMX stand-in" (`cluster_authorize.h:7`) |
| **MODPARAMS.DAT + AUTOGEN** cluster params | **UNSCOPED** — mentioned in boot design only |
| **CLUSTER_CONFIG(_LAN).COM** operator procedure | **UNSCOPED** — no item, only a comment in `cluster_authorize.h` |

The wire/RE half is owned by the cluster-interop program (`vms-ci`/`vms-694`);
the `.PAR` boot mechanism is owned by `vms-46c`. This epic owns only the
**operator-facing authoring surface** and depends into those two.

## The rungs (each a demonstrable release proof)

Principle (operator 2026-08-13): each minor release **demonstrates what
already exists in code**. Acceptance of each rung *is* a console/CI proof, not a
separate tracking item.

- **R1 — cluster SYSGEN params authored the VMS way, adopted on reboot.** `→ 0.5`
  SYSGEN.EXE / SYSMAN PARAMETERS SET/SHOW/WRITE for the cluster param set
  (SCSNODE, SCSSYSTEMID, VOTES, EXPECTED_VOTES, RECNXINTERVAL, ALLOCLASS),
  persisted to the `.PAR` store. **Demonstrates the `vms-ci.8` reading that
  already ships.** Deps: `vms-8da` (type-aware strings), `vms-46c` (.PAR write),
  `vms-41d` (VOTES reconciles on the VC). Proof: set params → WRITE CURRENT →
  reboot → `scsd`/`SHOW CLUSTER` reflect them, bracketed against a control.

- **R2 — MODPARAMS.DAT + AUTOGEN drive cluster params.** `→ 0.5`
  The VMS feedback pipeline: edit MODPARAMS.DAT, run AUTOGEN, a new `.PAR`
  version is generated and adopted at boot. Dep: R1.

- **R3 — real CLUSTER_AUTHORIZE.DAT: group# + password → grounded credential.**
  `→ 1.0` Replace the minimal stand-in with the real on-disk format and an
  authoring path (SET CLUSTER-authorize style); the HELLO credential is derived
  by the grounded hash so OVMX can join an **arbitrary** cluster, not just lab
  group 1. Deps: `vms-732` (hash from public docs), `vms-405` (must-auth call).

- **R4 — CLUSTER_CONFIG_LAN.COM provisions OVMX into a cluster, end to end.**
  `→ 1.0` (capstone) The interactive `@SYS$MANAGER:CLUSTER_CONFIG_LAN.COM`
  (ADD/CHANGE/CREATE) that drives R1–R3; operator runs it, the node JOINS a
  real VMScluster, `SHOW CLUSTER` shows it. Deps: R1, R2, R3, `vms-694`
  (join/rejoin), `vms-8d4` (SHOW CLUSTER facade-kill).

## Milestone ladder

| Milestone | Lands | Demonstrates |
|---|---|---|
| **0.5** | R1, R2 | operator authors cluster identity/votes the VMS way; node adopts on reboot |
| **1.0** | R3, R4 | arbitrary-cluster authentication + one-command `CLUSTER_CONFIG_LAN.COM` provisioning → join |

## Operator-reserved call (default chosen, proceeding)

**Does 1.0 require arbitrary-cluster CLUSTER_AUTHORIZE authenticity (R3), or may
1.0 ship lab-group replay only?** Default taken: **R3 is a 1.0 bar** — the stated
objective is joining an *arbitrary* VMScluster (total commoditization), and
lab-group-1 replay already works today. Flag to revisit if 1.0 scope tightens.

## Clean-room (Rule 8)

CLUSTER_AUTHORIZE hash, MODPARAMS/AUTOGEN behavior, and CLUSTER_CONFIG_LAN.COM
question flow are derived from public OpenVMS docs (Cluster Systems manual,
SYS$MANAGER guides) + lab wire observation only. OVMX authors all `.COM`/`.DAT`
content; OVMX-defined byte layouts are labelled as OVMX design choices.
