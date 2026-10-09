# vms-f87 — the queued VMS waiter nobody told, and the CPU that span

Two faults from the 2026-10-09 evacuation runs, both in the same place in the
story: a real VAX `$ENQW` queued at an OVMX master.

## 1. The waiter was never told (ci6-evac-9, 10:56Z)

OVMX held EX on `EVAC$WORKLOAD` as directory + master. VAX1's `$ENQW` EX **queued
at the OVMX master** — correct, a real lock on a real waiting queue. OVMX
released its EX, the engine flipped VAX1's request to GRANTED **for real**, and
**nothing went on the wire**. VAX1's process sat in `RWSCS` indefinitely; even
`STOP` could not complete it.

The arm had counted that as `deferred_grants_owed` and stayed silent, on the
reasoning that a `cat-0x82` at a system that "did not just ask" is uncorrelated
and ungrounded, and that the waiter's own retransmit would come back. **The lab
falsified the second half**: the VAX never asked again.

### What the captures say about the first half

`tools/cluster/dlm_grant_correlation.py` (new, with a selftest) measures it over
the 129 real VAX captures in the corpus:

| fact | measurement |
|---|---|
| how a grant is correlated | the **requester's own handle** at `body[24:28]`, which the master echoes back — **27,513 of 27,513** grants in the f03 reference capture matched that way |
| must it be the next frame? | **no** — gaps up to **137 ms**, with other frames in between |
| does a real master send grants nobody just asked for? | **yes** — **139 inside one second** of that capture, in exactly the shape every other grant has (the record at `body[28]`/`body[32:36]`, `body[30]` cleared, no name) |

**Honest gap, not papered over:** no capture in the corpus contains a
cross-node queued waiter at all, so the *context* "a master sends a grant when
its queue advances" is **not** directly captured. The 139-grant burst is a
rebuild/remaster, and reading it as a queue advance would be an inference across
contexts — the mistake that produced the "17K grant storm" misreading. What IS
grounded is the frame and the correlation; the behaviour is for the lab to
confirm. **The capture that would close it is specified below.**

### The fix

The grant is **the requester's own queued frame, echoed back** with the handle
this engine assigned (`vms_dlm_pending.h` keeps it from the moment the engine
queues the request). So it is byte-for-byte the grant that request would have
got had it been grantable at once — proven in
`tests/cluster/host/test_dlm_pending.c` — and not a frame OVMX composed from
fields, which is what rd vms-b5b0's 65,000-frame storm was. A flip whose request
frame is no longer held sends **nothing**, counted
(`deferred_grants_owed`), exactly as before.

## 2. The CPU that span (ci6-evac-11, 11:12Z)

Same shape, one step earlier: the **OVMX standby's own** `$ENQW` CONVERT NL->EX,
on a resource this node mastered, whose blocker was the remote VAX EX holder.

```
rcu: INFO: rcu_preempt self-detected stall on CPU 0 (9931 ticks this GP)
   ... 39649 ... 69348 ... 98763 ticks
```

CPU 0 never left the kernel; the fork thread on CPU 1 served cluster traffic
normally for two more minutes. **Root cause:** `exec_cv_wait_timeout` does not
sleep while a signal is pending (it would be woken again at once) and
`enq_wait_sync` **dropped that return** — so it re-tested a predicate that was
still false and called straight back in. A tight loop taking and dropping
`res->lock`. **Any** signal to a process blocked in `$ENQW` did it, including the
`STOP` sent to recover the process.

**The fix, and it is the contract the rest of this executive already uses**
(`vms_eflag.c`, `vms_mbx.c`, `$HIBER`): the ioctl ends with `-ERESTARTSYS` and
**no status**, the request stays queued, and userspace **re-enters the wait**
(`KIF_WAIT_CALL`, now used by `$ENQ`/`$CONVERT`). That is what VMS does when an
AST interrupts a wait: the AST runs and **the wait resumes** — which is why
`$ENQW` has no "your wait was interrupted" condition value to report.

Tested at R1 with the host backend's new **interrupt seam**
(`exec_host_interrupt_waits`), on a thread joined with a deadline, so the
regression is a named failed assertion in seconds instead of a hang.

## The capture this still needs (please run it)

Nothing in the corpus has a cross-node **queued** waiter. On the 2-VAX
reference cluster:

1. `tcpdump -i br0 -s0 -w f87-contention.pcap ether proto 0x6007` on the bridge;
2. on **VAX1**: `$ ENQ/MODE=EX OVMXF87A` (hold it — any driver that takes EX and
   waits, e.g. the existing `dlm-drivers` pattern in
   `tests/lab/captures/vms-c03-dlm-opcodes-20260911/dlm-drivers`);
3. on **VAX2**: `$ ENQW/MODE=EX OVMXF87A` — it must **queue** (SDA `SHOW LOCKS`
   on the master shows it on the conversion/waiting queue);
4. wait ~10 s (so the gap is unmistakable), then on **VAX1**: `$ DEQ` the lock;
5. stop the capture when VAX2's `$ENQW` returns.

The frame to look for is a `cat-0x82 op-0x01` from the master to VAX2 **~10 s
after** VAX2's `op-0x01`, with VAX2's own handle at `body[24:28]`. Run it through
`tools/cluster/dlm_grant_correlation.py` — a row with `gap≈10s` and
`release_between` naming VAX1's `op-0x03` is the grounding, and the shape can
then be diffed against what OVMX now sends.

## Lab acceptance for the two fixes

- a VAX `$ENQW` queued at an OVMX master is **granted within 1 s** of the OVMX
  `$DEQ`, with `%CNXTRACE-I-DLMRECV, ... deferred grants sent=1 owed=0`, the VAX
  process leaving `RWSCS`, and SDA `SHOW LOCKS` on the VAX showing the granted
  mode;
- an OVMX process's `$ENQW` CONVERT behind a remote holder **survives a signal**:
  `STOP` the process and the node stays up, no RCU stall, the request still
  queued (or the process deleted cleanly);
- 0 bugchecks on either VAX.
