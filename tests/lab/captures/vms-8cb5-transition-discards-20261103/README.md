# vms-8cb5: a state transition discards the directory entry, and OVMX removed it anyway

The V0.7-15 visitor gate (openvmx-site 41557352344, main 500f19a1a, order
`c-first-legacy`: C, A, B, 5 s apart) took VAXC (OpenVMS VAX V7.3) down with
LOCKMGRERR right after `%CNXMAN, proposing addition of system OVMXB`. It was
reproduced in the cluster lane's own pod (ovmx-ci/p0-8cb5-gate, the same
payload and the same page, with the workflow's `capture=true` tap). This is that
run.

| file | |
|---|---|
| `crash-frames.jsonl.gz` | every 0x6007 frame the page's L2 switch carried (gzip, so the corpus censuses that walk `*.jsonl` keep their pins) |
| `VAXC.console.log` | the bugcheck: LOCKMGRERR, PC 80A07362, R0=1 R1=0x84 R7=0x34 (the same PC as the vms-c55 specimens) |
| `OVMXA/OVMXB.console.log` | the OVMX consoles |

## The frames (indices into the decompressed file, read with `tools/cluster/transition_dlm_census.py`'s `frames()`)

```
1722  OVMXA -> VAXC  02/01  lookup  RMS$ fid 80 on OVMXASYS (26-byte name, group 0, mode 1)
1724  VAXC  -> OVMXA 82/01  body[34]=0xf9: nobody masters it -- OVMXA does; VAXC records the entry
~1777..2697          epoch 4: OVMXB's addition. A full rebuild: VAXC re-registers ITS
                      resources with 02/0d at step 5; OVMXA re-registers nothing (OVMX sends no 02/0d)
2741  OVMXA -> VAXC  02/04  removal of RMS$ fid 80 on OVMXASYS   <- an entry VAXC no longer holds
2743  VAXC  -> OVMXA 02/19  17 ms later
2755  last frame VAXC ever sends; LOCKMGRERR
```

Phase 1 of every transition discards all directory information cluster-wide
(Davis p. 7-40/7-41, p. 6-33) and each master re-registers its own entries.
OVMX's record that "VAXC holds my entry" (`dir_told_csid`, rd vms-c55) outlived
the discard, and because the vector still named VAXC the vms-c55 check passed.
So this is the same class rd vms-c55 closed for a resource mastered before the
vector existed, reached by a second road: a removal for an entry no directory
holds draws op 0x19 and the bugcheck.

## Measured

Pod runs of `c-first-legacy` at 500f19a1a: 1 LOCKMGRERR in 9. Over those nine
captures, an OVMX op-0x04 removal whose 0xf9 answer came BEFORE a transition
window (cat-0x01 EPOCH/STEP, as transition_dlm_census reads them) and whose
removal came AFTER it occurs 1 time, in the crashing run and in no other
(1,000+ other OVMX removals in the clean runs, none of them straddling a
transition). The integrator saw the same LOCKMGRERR on V0.7-14
(`page-order-throttled4`, openvmx-site 41568815394), so this is not a
V0.7-14..V0.7-15 regression. `dir_told_csid` dates from #1776 (V0.7-12).

## The fix

`vms_lock_dlm_directory_discarded()` (vms_lock.c), called at the start of
every transition (vms_dlm_scs.c `dlm_arm_transition_begin`), forgets every
told-directory record. Tests: test_lock_proxy
`p8cb5_no_removal_after_a_transition_discarded_the_entry`, test_dlm_scs_arm
binding check, host negctl `dir-told-survives-transition`.
