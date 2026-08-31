# Booted OVMX joins a real VMScluster — the real cluster-1.0 gap (vms-110b)

> Build plan / outcome tree. Grounded on `origin/main` (HEAD 0c9d15a7),
> 2026-08-31. Clean-room (Rule 8): SCS/NISCA/MSCP behaviour is from lab
> observation + public OpenVMS docs; no VSI bytes.

## The honest floor (what this plan fixes)

A **booted OVMX runtime is not a cluster node today, and never has been.** The
cluster capability was built as two halves that never meet in the shipping
runtime:

- **Real:** the SCS wire stack (`src/vmsscs/scs_hello.c`, `scs_quorum.c`,
  `scsd.c` join FSM) and the executive membership block (`src/kernel-core/
  vms_lock.c`, populated via `VMS_IOCTL_CLUSTER_MEMBER_SET/GET`). The userspace
  `SCSD.EXE` *probe*, hand-launched onto a lab pod's `br0`, genuinely joins a
  real VAX cluster.
- **Not built:** nothing starts SCS on a booted OVMX. `distro/rootfs/.../
  SYS$STARTUP/VMS$VMS.DAT` registers exactly one STDRV component
  (`JOB_CONTROL`); `src/ovmx_init/ovmx_init.c:953` merely *stages* `SCSD.EXE` as
  an on-demand image, never spawns it. So a booted node never populates the
  membership block from a real join, and never emits a HELLO on its NIC.

The register overclaim ("Proven on real 2/3-node QEMU") was excised in #998 —
the QEMU proof is single-node ioctl set/read-back, not booted nodes joining.

## The target

1. A **booted OVMX QEMU** joins a **real VAX** cluster end-to-end: `SHOW CLUSTER`
   *on the VAX* names the OVMX node MEMBER, and `SHOW CLUSTER` on OVMX (reading
   its own executive block, populated by its **own** booted scsd) names the VAX.
2. On top of that, the booted OVMX **serves** an MSCP$DISK unit the real VAX
   **MOUNTs** (this is vms-36a8 / vms-600's serve half, re-scoped onto a booted
   node instead of the probe).

## Why the pieces are real, not fanciful (grounded seams)

- `scsd` already runs **persistently**: `duration=0 → "run until SIGINT/SIGTERM"`
  (`scsd.c:15647`, `:16010`). A `RUN/DETACHED` system process is feasible now.
- `VAXCLUSTER`/`VOTES`/`EXPECTED_VOTES` are **already authored** into
  `OVMXVMSSYS.PAR` the VMS way and read back by SYSGEN SHOW — but only
  `SCSNODE`/`SCSSYSTEMID` are *consumed* today (`CLUSTER_CONFIG_LAN.COM` header).
  The work makes `VAXCLUSTER` effectual (gates auto-start).
- The STDRV component mechanism is **proven** — `JOB_CONTROL` is registered and
  `test_job_control_negctl.sh` proves removing its line stops it. We add an
  analogous SCS line.
- `run-qemu.sh` **already** supports `OVMX_NET_MODE=tap|bridge` for real L2 reach
  (`:139`/`:143`); the NIC surfaces as `ETH0:` in the executive (`vms_devtab.c`,
  vms-9d2). SLIRP is only the default.
- The executive membership block **already** accepts `scsd`'s publishes; it is
  simply never fed by a booted scsd today.

## Outcome tree (parent: vms-110b)

**110b.1 — A booted OVMX auto-starts SCS as a system process.**
Register an SCS STDRV component in `VMS$VMS.DAT` + a `JOB_CONTROL`-style startup
COM that: reads `VAXCLUSTER` from `OVMXVMSSYS.PAR` (≥2 ⇒ participate), resolves
the cluster NIC (`ETH0:` → its backing Linux ifname), and `RUN/DETACHED`s
`scsd --connect --iface <dev>` (persistent). Makes `VAXCLUSTER` effectual.
*Proof (single-node QEMU, no VAX needed):* boot with `VAXCLUSTER=2`; the scsd
system process is running bound to `ETH0:` and has published the local node into
the executive membership block (`SHOW CLUSTER` reflects it via `/dev/vms`).
*Negctl:* `VAXCLUSTER=0` ⇒ no scsd (mirrors `test_job_control_negctl.sh`).
*Cascade:* boot/init surface change → API-compat + test-coverage folded in.

**110b.2 — Cluster boot networking + mixed-topology lab harness.**
A `run-qemu` cluster mode that puts `ETH0:` on a shared L2 (tap/bridge), plus a
harness that boots **1 booted-OVMX QEMU + 1 real VAX (SIMH)** on one `br0` inside
a pod. *Proof:* a pcap on the shared segment shows the `0x6007` LAVC HELLO
multicast from **both** the OVMX node and the VAX. Depends on 110b.1 for
scsd-at-boot. **This is the corrected "N vms + M vmx" spinner the operator asked
for — booted OVMX nodes, not probes.** Delivered as a skill once it lands.

**110b.3 — End-to-end join (closes vms-110b).**
A booted OVMX joins the real VAX cluster. *Proof:* `SHOW CLUSTER` on the VAX
names the OVMX node MEMBER; `SHOW CLUSTER` on OVMX names the VAX; pcap of the
3-party join. Depends on 110b.1 + 110b.2. This is the honest cluster milestone.

**110b.4 — Booted OVMX serves MSCP$DISK a real VAX MOUNTs (= vms-36a8 re-scoped).**
On the booted node set `OVMX_MSCP_SRV_UNIT_FILE` to a genuine ODS-2 image
(`tests/ods2/real_vax_ods2.dsk`); the real VAX `MOUNT`s it. Finish `ONLINE-END`
(still `stub`, `design-mscp-direction.md:18-19`); ground the block-transfer
`+4/+6` header constants (the one genuine RE unknown) from a capture of the
**booted** node serving. *Proof:* `%MOUNT-I-MOUNTED` + `TYPE` of a marker on the
VAX console; pcap `ONLINE→ONLINE-END→GUS→READ→block-transfer` originating from
OVMX. Depends on 110b.3 (a node must join before it can serve).

## Dependencies

```
vms-110b (parent)
  110b.1  auto-start SCS at boot          (no dep; single-node provable)
  110b.2  cluster L2 + mixed lab harness  (dep: 110b.1)
  110b.3  booted OVMX joins real VAX      (dep: 110b.1, 110b.2)  ← closes vms-110b
  110b.4  booted OVMX serves, VAX MOUNTs  (dep: 110b.3)          ← closes vms-36a8/vms-600 serve half
```

## Open decision (operator)

110b.1 touches `ovmx_init`/STARTUP, adjacent to the conductor's kernel-core/CHF
lane. Does the cluster lane take vms-110b whole, or split the boot-integration
(110b.1) with the conductor? Everything from 110b.2 on is squarely cluster-lane.
