# vms-1b68 fire: REFUSED at the frame-conformance gate (2026-11-02), no OVMX node booted

`run_gate.sh` over the fire's 26 types (lab93r's list, incl. dlm_rm_08, dlm_rm_08_ans,
dlm_dir_ans_you, dlm_enq_ans_queued, dlm_grant_notify) PASSES on b3bbf8129, but only because no
emitter dumps the record shape this fire exists to send: `test_dlm_rebuild` calls `dump08()` for
granted records only, and the new waiter/converter tests (test_a_queued_waiter_is_rebuilt,
test_this_node_masters_the_waiter) dump nothing.

Dumping the waiter record that test_a_queued_waiter_is_rebuilt asserts (op 0x08, [30]=05 [31]=00
[110]=ff [108]=04; one temporary `dump08(g_sent[1].body, w, ...)` line, reverted) and gating it:

```
FAIL frame 10 (dlm_rm_08): abs 180+2 = 0400 fails row 496 (const const:0): == 0x0
FAIL frame 10 (dlm_rm_08): abs 182+1 = ff fails row 497 (const const:1): == 0x1
```

(A third failure, valblkseq at abs 176, comes from the temporary dump's own state argument, not
the frame.)

So frame-grounding.tsv still pins dlm_rm_08 body[108:110] const 0 and body[110] const 1 (the
granted-only census), and rows 102/103 still name body[30] the GRANTED mode and body[31] the
requested one, the reverse of the vms-0db verdict. The emitter's state string takes grmode/rqmode
from b[30]/b[31] (the frame itself), so the gate cannot see that swap either.

Needed before this fire (on work/vms-1b68-standby-takeover, not here): frame-grounding rows for
dlm_rm_08 (and dlm_rm_12) with [30]=requested, [31]=granted, [108]=04 iff non-granted, [110]=queue
(01/00/ff), grounded on the vms-0db capture census; and emitter dumps of a waiting and a converting
record whose state string is read from the LKB, not from the frame.

Also observed: pod lab1b68 (requests == limits, 1500m) stayed Pending, "0/3 nodes are available:
1 Insufficient cpu", and was deleted without ever running.
