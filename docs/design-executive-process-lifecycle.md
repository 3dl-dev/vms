# The executive owns the process lifecycle, behind one transport seam (rd vms-9f32, vms-bbde)

Status: DESIGN, for review before implementation. Epic rd vms-9f32 (p0). Symptoms
that led here: rd vms-d9ab, and the leader-zombie rundown bug found in #1549.

## 1. What VMS does

On OpenVMS a process exists in the executive from the moment it is created until it
is deleted, and the image it runs has no say in either:

- **Creation.** `$CREPRC` builds the PCB/JIB, assigns the PID, and stamps the
  identity, quotas and privileges *before* the image starts. LOGINOUT runs in a
  process that `$CREPRC` (from the terminal driver or JOB_CONTROL) has already
  created. STARTUP runs in the SWAPPER/STARTUP process that system initialization
  creates. No image ever "registers" itself.
- **Deletion.** `$DELPRC`, or image exit followed by process exit, runs process
  rundown in the executive: it deassigns channels, dequeues locks, releases
  common-EF associations, deletes the process logical names, delivers the
  termination message or completion to the creator, and frees the PCB. This
  happens however the process ends, and whether or not the image ever issued a
  system service.

## 2. What OVMX does today (measured on origin/main 5d471b5d1)

| Substrate | PCB created by | PCB deleted by |
|---|---|---|
| Linux (x86_64, aarch64, Alpha) | **Opt-in.** A task registers itself with `VMS_IOCTL_REGISTER*`. libvmssys `kif_bind()` does this lazily on the first `/dev/vms` call (`vms_kif.c`). `vms_dev_open` is a no-op. The `$CREPRC` child registers itself before exec (`REGISTER_SUBPROCESS`/`_DETACHED`, or the `CREPRC_TICKET` claim). DCL's fork+exec image uses `REGISTER_CONTINUE`. | **The last close of a `/dev/vms` file the process held**, in `vms_dev_release` (and only if the closer was the last live thread), or else the **lazy reaper** `vms_proc_reap_dead()`. The reaper runs on process-table ioctls and counts a zombie leader as alive. |
| NetBSD (amd64, VAX) | **Implicit opt-in.** `vms_proc_get()` find-or-creates a PCB on any facility ioctl from a pid. | **Only the lazy reaper** (`proc_find` skips zombies). PR #1587 adds an `exithook(9)`. |

Consequences, all observed:

- **Leader-zombie (#1549).** On Linux, if the last thread to drop `/dev/vms` is not
  the group leader, `vms_dev_release`'s `thread_group_empty()` test fails. The PCB
  then outlives the process, and the creator's armed completion never fires. (#1549
  patches the test to use `signal->live`; that is a fix to a mechanism this note
  removes.)
- **vms-d9ab.** An image that never opens `/dev/vms` itself (a foreign tool, or
  anything `$CREPRC` starts that makes no system service call) leaves its PCB
  behind. The `$CREPRC` child registered through the creator's O_CLOEXEC file, so
  nothing fires at exit, and the zombie keeps the PCB "alive" for the reaper.
- **NetBSD.** A creator polling its completion flag never sees a subprocess end
  (red in run 37940705927, green with the exithook in #1587).
- A process the executive never saw (any Linux task that has not yet made a
  `/dev/vms` call) has **no PCB at all**. It does not appear in `SHOW SYSTEM`, and
  `$GETJPI`, `$DELPRC` and `$FORCEX` cannot reach it.
- `vms_kif_getexit_linux`/`_pid` rely on the zombie keeping the row alive until the
  creator's `waitpid`. That is an artifact of fd/reaper deletion, not a VMS
  property.

## 3. The design

### 3.1 Creation: the executive creates the PCB atomically with the process

One executive operation, **`VMS_IOCTL_CREPRC_PCB`**, is issued by the **creator**
for a task it has just forked and is holding before exec. The operation:

1. Verifies the target tgid's `real_parent` is the caller (NetBSD: `p_pptr`), and
   that the target has no PCB.
2. Allocates the PCB with an executive-assigned PID.
3. Stamps identity, privileges, quotas and UIC from the creator's authenticated
   row plus the `$CREPRC` arguments, using the rules `$CREPRC` already applies.
   The child cannot change these.
4. Records the creator relationship: owner PID, subprocess vs detached, and the
   termination-notification target.
5. Returns the PID.

The child is released to exec only after this returns; the existing `$CREPRC` pipe
handshake reverses direction for this. Every creation path uses it:

| Path | Today | Under this design |
|---|---|---|
| `$CREPRC` / `LIB$SPAWN` / `RUN/DETACHED` | child self-registers (`REGISTER_SUBPROCESS`/`_DETACHED`, ticket) | creator issues `CREPRC_PCB` (the detached intermediate becomes the creator of record for the grandchild; the ticket goes away) |
| DCL fork+exec of an image (shares DCL's VMS PID) | child `REGISTER_CONTINUE` | DCL issues `CREPRC_PCB` with `CONTINUE`: an image-task row under DCL's PCB, not a second process |
| LOGINOUT | `$CREPRC`'d by JOB_CONTROL/terminal driver, or ssh | unchanged caller; the creator does the creation |
| STARTUP (PID 1) | registers through `kif_bind` | **the bootstrap exception, stated once:** the module creates the SYSTEM process PCB for the boot task at `ESTABLISH_SYSTEM` (callable once, by host-privileged PID 1), as VMS system initialization creates STARTUP |

**Registration by ioctl is removed.** `VMS_IOCTL_REGISTER`, `_SUBPROCESS`,
`_DETACHED` and `CREPRC_TICKET`, `kif_bind()`'s lazy register, and NetBSD's
find-or-create in `vms_proc_get()` all go. A task with no PCB that calls a facility
gets an honest refusal (`SS$_NONEXPR` from the facility, no fabricated row). This
follows Rule 9: a Linux task the executive did not create is not a VMS process.

### 3.2 Deletion: driven by the substrate's process-exit hook

When the **process** ends (its last thread exits), the executive runs process
rundown and deletes the PCB. This is one facility function,
`vms_proc_rundown(proc, status)`, shared by all substrates. It releases channels,
locks, common EF, LNM$PROCESS, ASTs, BG/L2/ACP channels, P0/P1 and cluster
registration (today's `vms_proc_free_claimed` body). It also delivers the
termination notification (§3.3) and frees the PCB. The hook depends on the
substrate:

| Substrate | Hook | Notes |
|---|---|---|
| Linux x86_64/aarch64 | `sched_process_exit` tracepoint, attached by name exactly as `src/kernel/vms_bg_forkinherit.c` already does (`for_each_kernel_tracepoint` + `tracepoint_probe_register`) | Fires per thread in `do_exit()` after `signal->live` is decremented (verified in linux-6.6.52 `kernel/exit.c`: `atomic_dec_and_test(&tsk->signal->live)` at line 836, `trace_sched_process_exit` at line 865, `exit_files` at line 869). Act when `atomic_read(&p->signal->live) == 0`, i.e. the group is dead. The probe runs in atomic context: unlink the PCB and deliver under the hash spinlock there, and do the sleeping parts of rundown (socket release, ACP) from a workqueue, as the BG fork-inherit records already are. |
| Linux Alpha | no tracepoints (CONFIG_TRACEPOINTS cannot build: no stack unwinder; see `vms_bg_forkinherit.c`) | **Proposal:** a one-hook kernel patch in `tools/cross-alpha/patches/`, which already patches the Alpha kernel (virtio_blk). It adds an exported notifier chain `ovmx_task_exit_notifier` called from `do_exit()` at the same point as `trace_sched_process_exit`. The module registers on it. Rejected alternatives: kprobes (not available on Alpha); the PROC_EVENTS connector (netlink to userspace, so the executive would depend on a daemon); `task_work` (runs on return to user mode, not only at exit). Optionally the same patch replaces the by-name tracepoint lookup on x86/aarch64 later, giving one mechanism for every Linux arch. |
| NetBSD amd64/VAX | `exithook_establish(9)` | Called from `exit1()` after `exit_lwps()` and `fd_free()`, in process context (may sleep). Implemented and proven in PR #1587 (it becomes the rundown caller). |

**Removed as lifecycle sources:**

- `vms_dev_release` deleting the PCB. Closing `/dev/vms` becomes what VMS would
  make it: nothing. `/dev/vms` is the system-service gate, not the process.
- `vms_proc_reap_dead()` and every call site (proctab, GETJPI, SPAWN_NOTIFY, the
  NetBSD glue). A PCB whose process is gone cannot exist, so there is nothing to
  reap. `vms_proc_task_alive()` remains only as an assertion.
- pid-recycling guards that exist because rows outlived processes (the `pid_ref`
  match in `vms_proc_find_or_err`) become invariants rather than runtime checks.

### 3.3 The creator learns how its subprocess ended: termination record, not a surviving PCB

Today DCL and `ovmx_spawn_pipeline` read a child's `$STATUS` by keeping the zombie's
row alive until `waitpid`. On VMS the creator learns this from the termination
message (`$CREPRC mbxunt`) or the completion (`LIB$SPAWN` `completion-status-address`),
both delivered at deletion. Under this design, rundown:

- delivers the armed `/NOWAIT` completion (EF + AST) and writes the subprocess's
  final status to the creator's `completion-status` request, if one was armed;
- posts a **termination record** (PID, final condition value, accounting) to the
  creator's PCB, in a small bounded ring per creator. `vms_kif_getexit_pid` and
  `_linux` read from that ring instead of a dead process's row. A `$CREPRC`
  termination mailbox, when one was given, receives the VMS-format termination
  message. That message layout must come from a real-VMS capture (oracle first),
  in its own item.

Exit status is recorded where it is today (`SETEXIT` at image rundown). A process
that ends without one gets `SS$_ABORT`, as `vms_proc_deliver_abnormal_completion`
already does.

### 3.4 What a thread, a fork, and an exec are

- A **thread** is never a lifecycle event (the tgid keying stays).
- **exec** is image rundown/activation inside the same process (`ENTER_IMAGE` /
  `image_rundown`), never a PCB event.
- A plain Linux **fork** from a VMS process (C RTL `fork()`, `system()`) creates no
  VMS process unless the creator issues `CREPRC_PCB`. DEC C `vfork`/`exec` goes
  through `$CREPRC` (Alpha `decc$` already does). The `sched_process_fork` hook may
  be used to carry BG-channel inheritance as today, but never to mint a PCB.

## 4. Audit: executive objects whose lifetime is tied to `/dev/vms` open/close or to opt-in

| Object | Where | Lifetime today | Under this design |
|---|---|---|---|
| Process PCB (identity, privileges, PID, name) | `vms_module.c` `vms_proc_register`, `vms_dev_release`; `vms_proctab.c` reaper; NetBSD `vms_proc_get` | opt-in register / find-or-create; deleted at last fd close or lazily | `CREPRC_PCB` / `ESTABLISH_SYSTEM`; exit hook |
| Image-task rows (`REGISTER_CONTINUE`, shared VMS PID) | `vms_module.c`, `dcl_cmd_process.c` | child registers itself; deleted like a PCB | created by DCL at fork; deleted at that task's exit as image rundown of the shared process |
| `$CREPRC` detached tickets | `vms_proc_creprc_ticket_*` | claimed by the grandchild's self-registration | removed (creator-driven creation) |
| Event flags: local cluster, common-cluster associations | `vms_eflag.c`, `vms_proc_release_common_ef` | PCB lifetime (so opt-in/fd) | PCB lifetime (exit hook) |
| ASTs queued per mode | `vms_ast.c` | PCB lifetime | PCB lifetime |
| Lock-manager locks held | `vms_lock.c`, `vms_proc_release_locks` | PCB lifetime | PCB lifetime |
| Mailbox channels; temporary mailboxes (deleted with the last channel) | `vms_mbx.c` via `vms_proc_release_channels` | PCB lifetime | PCB lifetime |
| Device channels and `$ALLOC` ownership | `vms_devtab.c` | PCB lifetime | PCB lifetime |
| Files-11 ACP file channels | `vmsfs_acp.c` `vms_acp_release_all` | PCB lifetime | PCB lifetime |
| BGn: sockets | `vms_bg.c` `vms_bg_release_all`; fork snapshots in `vms_bg_forkinherit.c` | PCB lifetime; fork snapshot freed by `sched_process_exit` (already exit-driven) | PCB lifetime; snapshot unchanged |
| L2 datalink handles | `vms_l2.c` `vms_l2_release_all` | PCB lifetime | PCB lifetime |
| LNM$PROCESS / LNM$JOB tables | `vms_lnm.c` `vms_lnm_proc_gone` | PCB lifetime (image vs process by shared-PID heuristic) | PCB lifetime; image rundown explicit at `image_rundown` |
| P0/P1 regions, image-active state | `vms_p1.c`, `ENTER_IMAGE` / `image_rundown` | P1 freed at PCB free; image state needs IMGACT's opt-in rundown call | image rundown at exec/exit by the hook; P1 with the PCB |
| `/NOWAIT` completion arm, exit status | `vms_proctab.c` spawn_notify / setexit | delivered at SETEXIT or at fd-release/reaper | delivered by rundown (§3.3) |
| `$SETCLUEVT` registration | `vms_cnxman_proc_gone` | PCB lifetime | PCB lifetime |
| JIB quotas | `struct vms_jib_quota` in the PCB | PCB lifetime | PCB lifetime (detached = own JIB, subprocess = creator's) |
| Userspace: per-thread `/dev/vms` fd + `kif_bind` lazy registration | `libvmssys/vms_kif.c` (`__thread vms_dev_fd`, `vms_bound_pid`) | opens and registers on first call | opens the gate only; never registers |
| LNM arena mmap | `vms_lnm_mmap` | per-open read-only mapping | unchanged (a view, not an object) |

Every per-process object above inherits the PCB's lifetime, so it is fixed by
§3.1–3.2 without per-facility changes. Each facility's release must be safe to call
from rundown context (workqueue on Linux, exithook on NetBSD). The PCB itself, the
image-task rows, the tickets, the completion/exit-status path and the userspace
binding are the objects whose lifecycle mechanism changes.

## 5. The transport seam: `/dev/vms` becomes a two-file swap (rd vms-bbde)

`/dev/vms` will be replaced by real system-call entries. Section 3 already makes
the process come from the **task** and never from the file. This section makes the
transport itself replaceable.

### 5.1 Userspace: one gate

- Every executive call goes through `kif_transport_*`
  (`src/libvmssys/kif_transport_{linux,netbsd}.c`): `kif_xport_dev_open`,
  `kif_xport_ioctl`, `kif_xport_dev_close`. These become `kif_xport_call(service_id,
  in, out)` plus the arena map. Nothing above the transport names a device path, a
  file descriptor or an ioctl number.
- Measured bypasses to move behind it (rd vms-bbde):
  - raw `VMS_IOCTL_*` issuers: `imgact.c`, `imgact_acp.c`,
    `libdatalink/ovmx_datalink.c`, `ovmx_init/ovmx_boot_sysgen_acp.c`,
    `vmstcpip/sockets/vms_bgsock.c`, and the `vms_syscall.h` helpers;
  - direct `"/dev/vms"` opens: `imgact.c`, `imgact_boundary_audit.c`,
    `ovmx_datalink.c`, `ovmx_boot.h`, `ovmx_boot_acp_read.c`, and
    `ovmx_boot_{linux,netbsd}.c`.

  IMGACT and the boot bridge are freestanding, with no libvmssys TLS. They get the
  same transport TU compiled freestanding: one source, two link contexts, no
  second copy of the encoding.
- **CI gate** (`tools/ci/check_transport_seam.py`, in Core Gates): fails on any
  `ioctl(`/`kif_xport_ioctl(` with a `VMS_IOCTL_*` argument, and any `/dev/vms`
  string literal in code (comments excluded), anywhere outside
  `src/libvmssys/kif_transport_*.c` and the kernel trees. It runs on the tree and
  is a static code property, not a manifest. A negctl fixture (a TU that opens
  `/dev/vms`) proves the gate reddens.

### 5.2 Kernel: one service table in kernel-core

- `src/kernel-core/vms_services.c` holds **one table, `service id → { handler,
  in_size, out_size, flags }`**, shared by Linux and NetBSD. A handler has the
  signature `uint32_t handler(struct vms_proc *proc, const void *in, void *out)`.
  It never sees `struct file`, an ioctl number, `__user` pointers or `copy_*_user`.
- The **per-substrate entry** is the only transport code:
  - Linux: `vms_dev_ioctl` → `vms_service_entry(id, uarg)`;
  - NetBSD: `vmsioctl` → the same.

  The entry copies in, looks up the calling process **from the task**
  (`current->tgid` / `curlwp->l_proc`, the PCB §3 created; no PCB means
  `SS$_NONEXPR` unless the service is flagged `NOPROC_OK`, which only
  `ESTABLISH_SYSTEM` is), dispatches, and copies out. Today that switch is 110
  ioctl cases in `vms_module.c` and 183 in `vms_netbsd.c`, many with per-case
  copy-in. Those switches collapse into table entries. The big-I/O ops (mailbox
  WRITE/READ, `IOC_VOID` on NetBSD) carry their buffer in the service's in/out
  descriptor, so the size limit is the transport's concern and not the handler's.
- The service **id** is the existing ioctl NR byte plus magic (stable ABI). Adding
  the syscall later is one new entry function per substrate that calls
  `vms_service_entry`, and one `kif_transport_*` file that issues it.

### 5.3 No executive state keyed by a file handle

The process comes from the task (§3). Measured: `filp->private_data` is unused and
`vms_dev_open` is a no-op. The PCB's release in `vms_dev_release` is the only
fd-keyed lifetime, and §3.2 removes it. The LNM arena `mmap` is a read-only view
and survives as the transport's "map the arena" operation (a syscall transport
would map it from a known object). The gate in 5.1 plus a kernel-side review
check (no `struct file *` parameter outside the two entry files) keep it that way.

## 6. Implementation plan (reviewable PRs, each green on its own)

0. **Userspace transport seam + CI gate** (§5.1). Mechanical, with no behaviour
   change. Lands first so every later PR is written against the seam.
1. **Exit-hook rundown, no fd/reaper deletion.** One facility `vms_proc_rundown`.
   Linux: the `sched_process_exit` probe on group-dead, with rundown on a
   workqueue. Alpha: the exit-notifier kernel patch. NetBSD: #1587's exithook,
   moved onto `vms_proc_rundown`. Delete `vms_dev_release`'s free and
   `vms_proc_reap_dead`. Add the termination-record ring and switch
   `getexit_pid/_linux` to it. **Booted regression tests, each with a negctl
   anchor:** (a) vms-d9ab: `LIB$SPAWN`/`$CREPRC` of an image that never opens
   `/dev/vms`; the creator's armed flag is set and the PCB is gone. (b)
   Leader-zombie: a subprocess whose main thread exits first; flag set, PCB gone.
   (c) No-PCB-after-exit: `$GETJPI` of an ended subprocess is `SS$_NONEXPR` before
   the creator `waitpid`s. Negctls: skip the rundown call in the hook (reds a, b
   and c); keep a zombie "alive" (reds c).
2. **Creator-driven creation.** `CREPRC_PCB`, used by `$CREPRC`, DCL image-tasks
   and `ESTABLISH_SYSTEM`. Remove the `REGISTER*` ioctls, tickets, `kif_bind`
   registration and NetBSD find-or-create. Tests: a task the executive did not
   create gets `SS$_NONEXPR` and appears in no `$PROCESS_SCAN`; a `$CREPRC`'d
   child's PCB exists before its first instruction (`$GETJPI` from the creator
   between fork and exec). Negctl: a facility ioctl that mints a PCB for an unknown
   task.
3. **Harness migration** (lands with or before 2). The QEMU init.sh runs each suite
   as a process the executive created: a small `$CREPRC` launcher run by PID 1.
   Suites that fork children which call system services (mbx_prot, spawn_complete,
   eflag_mproc, ...) create them through `CREPRC_PCB`. This is the largest mechanical
   change: ~37 test sources register explicitly today.
4. **Kernel service table** (§5.2), split by facility group so each PR is
   reviewable: (a) the table, the entry and proctab/eflag/AST; (b) mbx/lock/LNM;
   (c) devtab/ACP/BG/L2/cluster. Each Linux and NetBSD switch case moves into the
   table in the same PR, so both substrates use one dispatch from the first PR on.
   This does not depend on 2/3 and can run in parallel after 1.
5. **Termination mailbox** (`$CREPRC mbxunt`): VMS-format message from a real-VMS
   capture first. Separate item.

Open questions for review: (i) the Alpha kernel patch vs. a different Alpha hook;
(ii) whether a plain C RTL `fork()` child should be refused outright or become a
subprocess implicitly (this note proposes refused: honest, as on VMS where `fork`
does not exist).
