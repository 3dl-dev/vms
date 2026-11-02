# RA7 fire C: an addendum, registered after fires A and B and before fire C boots

This adds an arm. It does not re-cut any prediction in [`PLAN.md`](PLAN.md):
fires A and B are scored exactly as that plan registered them.

## Why fire C exists

On the wire, fires A and B gave the two real VAXes different roles than
PLAN.md's table assumed. The directory node for `RA7T` is fixed by the name's
hash, and in this cluster it is **VAX2** both times.

| fire | fill | VAX1 receives an op-0x14 as | VAX2 receives an op-0x14 as |
|---|---|---|---|
| A | zeros | old master (not directory) | directory (not old master) |
| B | `0x5A` | **nothing.** VAX1 held nothing in the tree, so no 0x13 and no 0x14 (rule W1) | old master **and** directory |

So the pattern has reached one role only: old master and directory in a single
node. The old-master-only role has seen zeros and nothing else. That is the
same shape of gap `heldout-proof-must-vary-topology` warns about: a cell that
only the old-master-only role reads would still be hidden.

## The arm

Fire A's arrangement exactly (O = VAX1, N = OVMXE, directory = VAX2), on a
freshly reset pod, from the same artifacts and the same gated build
(`ef0b7e3fc`), with `RM_RESIDUE_FILL=0x5A`:

```
RM_RESIDUE_FILL=0x5A OVMX_FIRE_GROUP=1 tests/lab/tools/ovmx_fire.sh lab6ad1 <art> ra7c <build> \
    dlm_enq_req dlm_enq_ans_queued dlm_grant_notify dlm_deq dlm_rm_ack dlm_rm_ack_nm dlm_rm_lock_ans dlm_rm_14_ra7
```

## Predictions (scored against fire A, which is identical except for the fill)

- **C1.** VAX1 (old master) and VAX2 (directory) each answer OVMXE's op-0x14
  with an `82/15` that differs from the request only at body[0:4], [8], [9]
  and [34]. The `5a` span comes back verbatim.
- **C2.** VAX1 ends with no RSB for `RA7T`. VAX2's directory entry reads
  `CSID 00010003`, lock count 0, the same as fire A.
- **C3.** VAX2's `RMHOLD 100-RA7T` is granted CR by OVMXE, and VAX2's SDA shows
  the same resource state fire A's did (master `00010003`, one CR lock,
  value block and sequence as fire A's).
- **C4.** No bugcheck on any console, and `CLUSTER_NODES = 3` throughout.

FAIL/STOP is PLAN.md's: any difference from fire A that the fill causes makes
that cell meaningful, and the result goes to the conductor before vms-93fa.
