# Design — cluster membership crosses into the executive (vms-551)

> SHOW CLUSTER / $GETSYI / cluster-wide locking must see a REAL cluster through
> `/dev/vms`, not a userspace file. Today the live membership lives only in scsd's
> peer table and is published to a FILE (`/var/run/ovmx/cluster_state`), which
> SHOW CLUSTER reads — a userspace-to-userspace path that reports membership with
> nothing in the executive behind it (the INV-6/Rule-9 LARP class). This wires it:
> vms.ko owns a membership block, scsd populates it via ioctls, SHOW CLUSTER reads
> it back through `/dev/vms`.

## Conductor ruling + two refinements (banked)
- Membership CROSSES INTO THE EXECUTIVE (Rule 9, consistent with the now-done
  executive-resident DLM vms-7fa): vms.ko owns the block; scsd populates it; SHOW
  CLUSTER / $GETSYI read it through `/dev/vms`. No userspace fake that reports
  membership success without the executive behind it.
- **NOTMEMBER ≠ NOSUCHDEV** — DISTINCT statuses, never conflated: executive
  reachable with 0 members → `SS$_NORMAL`, view says NOTMEMBER (SHOW CLUSTER prints
  "NOTMEMBER"); executive UNREACHABLE (no `/dev/vms`) → `SS$_NOSUCHDEV` (2680).
- The block MUST be NEW. `dlm_member_csids` (vms_lock.c) is a STATIC 0444 insmod
  param, CSID-ONLY, for directory hashing — read-only and not populatable, and
  vms-50e/DLM is active on it. Keep the membership block SEPARATE (Rule 8:
  OVMX-internal; first SCSNODE-name membership state in the kernel).

## The executive block (kernel-core / vms.ko)

```c
struct vms_cluster_member {           /* one node the CM sees */
    uint32_t csid;                    /* cluster system id (SCS low-16 identity) */
    uint32_t sysid;                   /* SCSSYSTEMID */
    char     scsnode[16];             /* SCSNODE name, NUL-padded ("" until learned) */
    char     state[16];               /* "MEMBER", "BRK_NON", ... */
};
```
Module-global: `vms_cluster_member vms_cluster_members[96]` + a live count, under a
lock. Maps 1:1 to the file bridge's `struct scs_cluster_member` (node/sysid/state)
plus `csid`. Held in kernel memory so a reader on `/dev/vms` sees the SAME set every
process sees — the INV-6-decisive property (not a per-process fake).

## New ioctls (Design Change Cascade — clone the `VMS_IOCTL_DLM_MEMBER_DEPART` pattern)

Next free nrs (0x30-0x38 used): **0x39 SET, 0x3a CLEAR, 0x3b GET**.

| ioctl | nr | who issues | census |
|---|---|---|---|
| `VMS_IOCTL_CLUSTER_MEMBER_SET`   | 0x39 | scsd (direct POSIX ioctl) | kif wrapper OVMX-UNWIRED |
| `VMS_IOCTL_CLUSTER_MEMBER_CLEAR` | 0x3a | scsd (direct POSIX ioctl) | kif wrapper OVMX-UNWIRED |
| `VMS_IOCTL_CLUSTER_MEMBER_GET`   | 0x3b | SHOW CLUSTER (DCL, via kif) | kif wrapper WIRED (real issuer) |

- SET args: one `vms_cluster_member` (csid/sysid/scsnode/state) → insert-or-update
  by csid. CLEAR args: a csid → remove. GET args: an out array + count (the view);
  0 members is a valid `SS$_NORMAL` (NOTMEMBER), never an error.
- Mirror in BOTH `src/kernel/vms_ioctl.h` (Linux) AND `src/kernel-netbsd/
  vms_lock_nb.h` (NetBSD twin) with `_Static_assert` on struct size + `_IOWR`
  encoding (the #928 twin trap). Dispatch in both backends (`vms_module.c`,
  `vms_netbsd.c`). Prototypes in both `vms_internal.h`.
- kif wrappers in `src/libvmssys/vms_kif.{c,h}`: `vms_kif_cluster_member_set` +
  `_clear` declared **OVMX-UNWIRED** (scsd issues them directly, like
  `vms_kif_dlm_member_depart`); `vms_kif_cluster_get_members` is a **wired** issuer
  (SHOW CLUSTER calls it), which satisfies kif_caller_census for GET.

## scsd populate path (`src/vmsscs/scsd.c`)

`scsd_publish_membership` (the existing publisher, ~:14295, that writes the file)
ALSO drives the executive: for each live member SET; on a departure CLEAR. Keep the
file publish alongside for this slice (SHOW CLUSTER cuts over to the executive read;
$GETSYI + file-publish-retire are deferred). Direct POSIX ioctl on `/dev/vms`, same
as `scsd_emit_...` / the depart ingress — NOT a new SCS wire send, so NO CHOKED
SEND SITE TABLE entry (this is a LOCAL ioctl, not a peer send).

## SHOW CLUSTER read cutover (`src/vmsdcl/dcl_cmd_show.c`)

`cmd_show_cluster` (:2971) today calls `scs_membership_read(&view)` (the FILE).
Cut it over to `vms_kif_cluster_get_members(&view)` reading the executive:
- executive reachable, view.n_members == 0 → print **NOTMEMBER** (`SS$_NORMAL`).
- executive UNREACHABLE (`SS$_NOSUCHDEV`) → the existing no-/dev/vms behavior (as
  vms-8d4 already does), DISTINCT from NOTMEMBER.
- otherwise print the members (SYSTEMS/MEMBERS) from the executive view.

## Proof (real `/dev/vms`, 2/3-node QEMU, INV-6)

Reuse the DLM harness fabric (A/B[/C], real vms.ko on each). scsd on each node
populates the executive membership as the cluster forms; SHOW CLUSTER (or a
`test_syssvc_cluster_member` on real `/dev/vms`) reads it back and asserts:
- each node's executive view lists the live members (csid/sysid/scsnode/state),
  read from `/dev/vms`, matching what the cluster actually formed — reconstructed
  from REAL executive state, never a fabricated set.
- a node with the executive up but no cluster → NOTMEMBER (`SS$_NORMAL`, n=0);
  distinct from no-`/dev/vms` → `SS$_NOSUCHDEV`.
- a member departs → scsd CLEARs it → the executive view shrinks (getlki-style
  readback shows the real removal).

## Design Change Cascade (new ioctls)
1. **API-compat**: additive (3 new opcodes, new struct) — no existing ABI changed;
   `_Static_assert`s pin the encodings.
2. **Test-coverage**: the real-`/dev/vms` QEMU acceptance above (not a userspace
   unit test).
3. **Doc**: this design note + the compat surface (`docs/compat/facilities/
   cluster-*.yaml`) updated + `render_compat.py` if a member row is added.

## Deferred (Rule 5, file on land)
- `$GETSYI` CLUSTER_MEMBER/NODES cutover to the executive (still reads the file).
- Retire the file-publish bridge once all readers cut over.
- Unify with `dlm_member_csids` (collides with vms-50e's active DLM path).
- Votes / quorum / expected-votes state.
