# Executive kernel messages -> the operator surface (vms-32a)

**Status:** implemented. **Target:** `vms.ko` / `vmsfs.ko` printk records,
`src/ovmx_init/opcom_kmsg.{c,h}` (new), `src/libvms/syssvc/sys_operator.c`
(OPCOM record format). **Method:** lab-Alpha observation (OpenVMS Alpha V8.4,
`tests/lab-alpha/`) of the real boot-console and OPCOM message shapes, plus
CLAUDE.md Rule 8 (clean-room RE: behavior + public docs only, never VSI
source/binaries).

## 1. The problem

`vms.ko` and `vmsfs.ko` carry ~40 `pr_info`/`pr_warn`/`pr_err` calls. Every
one of them goes straight to the Linux kernel ring buffer (`dmesg`) and
nowhere else. OVMX has no VMS operator surface at all today (no OPCOM
process, no boot-console routing for kernel-module events), so:

- genuine executive lifecycle events (the disk unit table forming, the
  console terminal being created, the logical-name arena coming up) are
  invisible to a VMS operator -- they exist only in a Linux-only log a VMS
  session has no way to read;
- when anything DOES leak onto the physical console, it leaks as a raw
  `vms: ...` dmesg line or as SYSKRNL (Linux-kernel-layer) noise wearing no VMS shape at
  all (module-signature/taint warnings, `hrtimer: interrupt took ... ns`) --
  information an operator would actually want (a taint warning means an
  unverified executive image loaded; an hrtimer warning is a real host
  scheduling-latency signal), just in the wrong clothes.

## 2. Two message vocabularies, two surfaces (the oracle)

Observed on lab-Alpha (OpenVMS Alpha V8.4) across a clean boot -> login:

**(A) Boot/executive console lines.** Bare `%FACILITY-S-IDENT, text`, no
OPCOM banner, mostly no per-line timestamp. Chained conditions get a
`-FACILITY-S-IDENT, text` continuation line. Examples captured verbatim:

```
%STDRV-I-STARTUP, OpenVMS startup begun at 12-AUG-2026 13:21:20.22
%SET-I-INTSET, login interactive limit = 64, current interactive value = 0
%LICENSE-F-EMTLDB, license database contains no license records
```

There is no `%%%%%` banner anywhere in this vocabulary -- OPCOM is not
running yet when these print.

**(B) OPCOM records.** Post-boot, OPERATOR.LOG and the live enabled-terminal
delivery are the same shape:

```
%%%%%%%%%%%  OPCOM  12-AUG-2026 13:21:35.77  %%%%%%%%%%%
Message from user SYSTEM on ALPHA1
%JBC-E-OPENERR, error opening SYS$COMMON:[SYSEXE]QMAN$MASTER.DAT;
```

Banner = exactly eleven `%`, two spaces, `OPCOM`, two spaces,
`DD-MMM-YYYY HH:MM:SS.ss` (month upper-case 3-letter), two spaces, eleven
`%`. Body line 2 names the real node (SCSNODE), and for a numbered/repliable
request reads `Request <N>, from user <U> on <N>` instead. Live-terminal
delivery is separately gated by `REPLY/ENABLE` AND the terminal's BROADCAST
attribute -- neither of those is this item's scope.

**The decision this drives:** the ~40 kernel-module printks are all boot-time
executive events with no OPCOM process running yet, so they are routed as
vocabulary (A) -- bare console lines -- never wrapped in the (B) banner.
Only `sys$sndopr` (an existing, separate code path -- `src/libvms/syssvc/
sys_operator.c`) writes real (B)-shaped OPCOM records, and its bug was the
banner/timestamp/node SHAPE, not the vocabulary choice, so §4 below is a
format fix, not a re-routing.

## 3. Route by default; re-style, don't suppress (operator correction, 2026-08-12)

**Original decision (superseded).** The first pass of this item filtered by
a positive allowlist on the `printk` prefix: `vms:`/`vmsfs:`-prefixed
records routed, everything else -- including SYSKRNL (Linux-kernel-layer) lines like the
module-taint warning and `hrtimer: interrupt took ... ns` -- was suppressed
outright.

**Operator ruling, after review:** wrong call. Those SYSKRNL lines carry
*genuine operator-relevant information* -- a module-taint warning means an
unverified executive image loaded (a real security/integrity signal); an
hrtimer scheduling-latency warning is a real host-health signal. Dropping
them loses diagnostics a VMS operator would want. The revised rule:

> DEFAULT = re-style SYSKRNL/Linux lines into VMS-form operator messages
> that preserve the information; only drop lines with genuinely zero
> operator value, and when in doubt, ROUTE.

**What this item implements**, in `opcom_kmsg_classify()`:

- A record whose message text starts with `vms: ` or `vmsfs: ` (OVMX's own
  module prefixes, already on every line in `src/kernel/vms_module.c`,
  `vms_devtab.c`, `vms_lnm.c`, `vms_mbx.c`, `vmsfs/vmsfs_super.c`) is a
  genuine OVMX executive event -> routed under the **OVMX** facility, any
  severity up to kernel debug, exactly as before.
- Everything else is a **SYSKRNL (Linux-kernel-layer) line** -> re-styled and routed
  under the **SYSKRNL** facility (§4) when its kernel severity is NOTICE
  or more severe (`level <= 5`: EMERG/ALERT/CRIT/ERR/WARNING/NOTICE).
  Routine **INFO**-level device/bus-enumeration chatter (`level == 6` --
  ACPI, PCI, virtio, block-layer probe noise, hundreds of lines on a fresh
  boot) is the "genuinely zero operator value" bucket and is dropped: real
  VMS's own boot console does not surface internal driver-probe minutiae
  either, and mirroring the *entire* kernel ring buffer onto a VMS console
  would defeat the point of re-styling it as one. No per-string denylist is
  needed for this bucket -- it is a severity threshold, not a list of
  wordings to keep current.
- `level == 7` (kernel debug) is dropped for **both** facilities -- the
  routine per-process trace line (`vms: registered process ...`, lowered to
  `pr_debug` by vms-2213) stays off the operator surface for the same
  reason vms-2213 took it off the console, and routine kernel debug
  chatter is definitionally not operator-facing.

**The threshold is measured, not guessed**, against the two examples the
operator ruling named explicitly. Loading an unsigned out-of-tree kernel
module (every OVMX boot, until the modules are signed) makes **Linux
itself** -- not `vms.ko`/`vmsfs.ko` -- print its own generic taint warning
as `"%s: <text>"` with the *loading module's own name* substituted, and
that name is literally `vms`/`vmsfs` (a real, MEASURED prefix collision:
these two lines pass the `vms:`/`vmsfs:` prefix test by coincidence, not by
intent -- but since SYSKRNL lines now route by default rather than being
denylisted, the collision is harmless: it just means these two happen to
also match the OVMX-prefix branch. `opcom_kmsg_classify()` does not special-
case them; they are ordinary `vms:`-prefixed text and get the `KMOD` ident
like any other unrecognized `vms:` line):

```
vms: loading out-of-tree module taints kernel.                                          -> level 4 (WARNING)
vms: module verification failed: signature and/or required key missing - tainting kernel -> level 5 (NOTICE)
```

Both measured against a real boot (`tests/qemu/test_job_control_console.sh`).
hrtimer's scheduling-latency warning (`kernel/time/hrtimer.c`,
`printk_deferred(KERN_WARNING, ...)`) is level 4, the same side of the
NOTICE cutoff. `level <= 5` keeps both named examples routed and excludes
the routine-probe flood.

## 3b. Console destination -- three rounds, ending at OPERATOR.LOG-only

**Round 1 (superseded).** §3 above routes SYSKRNL lines "by default" -- but
round 1 wrote every routed SYSKRNL line straight to `/dev/console`,
alongside `vms.ko`/`vmsfs.ko`'s own `OVMX`-facility lines. That reopened
the exact leak PR #358 (vms-2213) closed: PR #358 deliberately keeps
routine kernel `printk` off OPA0: so the boot console matches the OpenVMS
oracle (`docs/design-boot-faithful.md`). CI measured the regression on a
real runner: its hardware/kernel combination emits far more NOTICE/
WARNING-level chatter than this repo's minimal dev QEMU guest ever showed
locally --

```
%SYSKRNL-I-KERNEL, Loaded X.509 cert 'Canonical Ltd. ...'
%SYSKRNL-I-KERNEL, Key type ... registered
%SYSKRNL-W-KERNEL, amd_pstate: the _CPC object is not present ...
```

-- flooding the console and stalling the boot before `Username:` (PR #358's
own "Persistent Boot Smoke" / boot-console-conformance CI job went red).

**Round 2 (also superseded).** The first fix looked like a DESTINATION
split by facility: `OVMX`-facility lines (`vms.ko`/`vmsfs.ko`'s own,
plus the prefix collision in §4) stayed on `/dev/console` -- reasoned as
"never the complaint, PR #358's baseline already expects these" -- while
`SYSKRNL`-facility lines moved to `SYS$MANAGER:OPERATOR.LOG` only.

**That reasoning was wrong, and CI caught it too** (a second red run on the
same PR): `tests/qemu/test_boot_conformance.sh` pins the EXACT ordered
sequence of `%FACILITY-SEVERITY-IDENT` tokens the console may show, derived
from the OpenVMS Alpha oracle, and that sequence is produced ENTIRELY by
the boot orchestrator (`ovmx_init`/`PROVISION.EXE`/`SYSTARTUP_VMS.COM`) --
never by a kernel module's own `printk`, regardless of which OVMX-style
facility label it wears. Round 2's `OVMX`-facility kmsg lines (`%OVMX-I-
DEVTAB`, `%OVMX-I-LNM`, `%OVMX-I-MBX`, `%OVMX-I-SYSID`, `%OVMX-I-KMOD`,
`%OVMX-I-VMSFS`) are exactly the SAME kind of extra tokens the pinned
sequence does not contain, for the SAME reason `SYSKRNL` lines were not
supposed to be there: they come from a kernel module's printk, not from
the orchestrator's own narration. Sharing a facility NAME with lines the
orchestrator genuinely does print (`%OVMX-I-EXEC`, `%OVMX-I-SYSDISK`, ...)
does not make a kernel-module printk part of that narration.

**Round 3 (current, definitive): the console is never a destination for
this bridge, full stop.** Not for `OVMX`-facility kmsg lines, not for
`SYSKRNL` lines. `opcom_kmsg_classify()` collapses from three outcomes to
two: `OPCOM_KMSG_DROP` or `OPCOM_KMSG_OPERATOR_LOG` (`opcom_kmsg.h`). The
severity threshold from §3 is unchanged -- SYSKRNL lines at NOTICE-or-worse
still route, INFO/DEBUG still drop -- only the destination collapses to
one answer. `opcom_kmsg_thread_main()` no longer opens `/dev/console` at
all; every non-dropped record goes straight to
`opcom_kmsg_append_operator_log()`, which resolves `SYS$MANAGER:
OPERATOR.LOG` through vmsfs -- the SAME accessor `sys_operator.c`'s
`sys$sndopr` uses -- falling back to `/tmp/OPERATOR.LOG` under the SAME
condition (system disk not yet mounted) that function already falls back
for. It opens, appends, and closes per write rather than holding one fd
for the life of boot, because the resolvable path can change mid-boot
(before vs. after the system disk mounts) and re-resolving each time is
how a write after the mount finds the real file without the bridge
needing to know when that happened.

**This does not relitigate the two-vocabulary model (§2).** These lines
still carry NO OPCOM banner -- they are not becoming `sys$sndopr` records,
and they do not claim to be OPCOM output. They simply now share
OPERATOR.LOG's FILE with real OPCOM records, bare-shaped, exactly as they
were always bare-shaped when round 1/2 imagined them on the console.
`OPERATOR.LOG` becomes, in effect, both "the OPCOM record log"
(vocabulary B) and "the boot-time kernel/executive history an operator
can search after the fact" (vocabulary A, persisted rather than
broadcast) -- a pragmatic exception the operator ruling explicitly asked
for ("mirrors real OPCOM log-vs-broadcast"), not a claim that VMS's real
OPERATOR.LOG mixes the two.

**Verification.** `tests/ovmx_init/test_opcom_kmsg.c` asserts the
two-outcome destination for every case, including the (former) console
cases from rounds 1-2, which now assert `OPCOM_KMSG_OPERATOR_LOG` instead.
`tests/qemu/test_job_control_console.sh` asserts, against a real boot:
NO line from either facility -- `OVMX` or `SYSKRNL` -- reaches the console
in any form (checked by ident and by the specific noise strings CI's two
failures quoted), the console's ordered facility+ident token sequence
matches the pinned oracle-derived shape exactly (the same check
`test_boot_conformance.sh` makes, corroborated inline), AND -- the positive
half -- OPERATOR.LOG (read back via `TYPE`, over the same live session)
actually contains the bridge's lines: MEASURED reliably present across
repeated manual boots (`%OVMX-I-SYSID`, `%OVMX-I-LNM`, `%OVMX-I-MBX`,
`%OVMX-I-VMSFS`, at minimum -- the very first record or two emitted before
vmsfs's path resolution comes up can still land in the `/tmp` fallback
instead, the same disclosed timing edge `sys$sndopr`'s own writer has
always had). `tests/qemu/test_boot_conformance.sh` itself -- the CI gate
that caught round 2 -- passes unmodified.

## 4. The facility code -- OVMX DESIGN CHOICE (Rule 8)

Loading a kernel module, registering `/dev/vms`, and the other kernel-module
lifecycle events these lines describe have **no real-VMS equivalent
wording** -- there is no VSI facility that means "a Linux LKM attached
itself to the process." Per Rule 8, OVMX does not invent VMS-authentic text
for a condition VMS never faces.

**Facility: `OVMX`.** This is not a new invention for this item -- it is the
SAME facility `src/ovmx_init/ovmx_init.c` and `src/ovmx_provision/
ovmx_provision.c` already use for exactly this class of message
(`%OVMX-I-EXEC, VMS executive attached on /dev/vms`, `%OVMX-I-SCSNODE, ...`,
`%OVMX-F-EXECINIT, ...`). Reusing it keeps one facility for "OVMX boot
orchestrator says something with no VMS analogue," rather than growing a
second one that means the same thing.

**Idents** (all OVMX-invented, labelled here so the choice is traceable):

| Ident | Covers | Example |
|-------|--------|---------|
| `KMOD` | `vms.ko` module lifecycle (init/attach/register/unload) and anything `vms:`-prefixed not covered by a more specific ident below | `%OVMX-I-KMOD, initializing VMS kernel module` |
| `DEVTAB` | disk-unit table entries, console-terminal creation | `%OVMX-I-DEVTAB, disk unit vda -> DKA0: (0:0)` |
| `SYSID` | the SYSTEM identity constant the executive establishes at module load | `%OVMX-I-SYSID, system identity constant SYSTEM [1,4] privileges=ALL established by the executive` |
| `LNM` | the logical-name arena | `%OVMX-I-LNM, logical-name arena ready (...)` |
| `MBX` | the mailbox table | `%OVMX-I-MBX, mailbox table initialized` |
| `VMSFS` | every `vmsfs:`-prefixed line (`vmsfs.ko` init/mount) | `%OVMX-I-VMSFS, mounted block device, volume '...'` |

**Severity letter** comes straight from the printk level the kernel already
assigned (`pr_err`=E, `pr_warn`=W, `pr_info`/`pr_notice`=I, emerg/alert/crit=F).
This is a mechanical, honest translation of a real severity the kernel
already computed -- not a judgment call OVMX is inventing.

**Content stays exactly what the module already says.** Only the format
changes (VMS facility-tagged line instead of a raw `dmesg` line); the
message text itself -- honest OVMX wording about what OVMX's own kernel
module did -- is passed through unedited apart from stripping the redundant
`vms: `/`vmsfs: ` prefix the facility tag now supersedes. This is never
presented as VMS-authentic text (Rule 8) -- it is OVMX's own facility
carrying OVMX's own honest description, in VMS's message shape.

**A second facility: `SYSKRNL`.** Re-styled SYSKRNL (Linux-kernel-layer)
lines (§3 -- `hrtimer: interrupt took ... ns` and anything else without a
`vms:`/`vmsfs:` prefix that clears the NOTICE-or-worse bar) wear a
*different* facility than OVMX's own records, so the two stay honest about
origin: `OVMX` means "OVMX's own executive/boot-orchestrator code said
this", `SYSKRNL` means "the Linux kernel layer underneath said this,
re-styled into VMS's message shape." The name is not a new coinage for
this item -- it reuses `docs/architecture.md`'s own **Product and Kernel
Layering** vocabulary (`OVMX_SYSKRNL_NAME`, "OVMX/Linux"), the SAME
distinction that banner already draws at the very start of boot. One
ident, `KERNEL`, covers
every `SYSKRNL` record -- deliberately not subdivided further per-driver
or per-subsystem (that would need parsing `/dev/kmsg`'s `SUBSYSTEM=`
dictionary field, which this item does not do; "don't over-build it" per
the operator ruling). Content is the raw kernel message text, unedited,
under the mechanical severity mapping above -- e.g. `%SYSKRNL-W-KERNEL,
hrtimer: interrupt took 45084781 ns`.

A `vms:`/`vmsfs:`-prefixed line that actually originated from the KERNEL
ITSELF, not from `vms.ko`/`vmsfs.ko` (the measured taint-warning collision,
§3), still wears `OVMX`/`KMOD` rather than `SYSKRNL` -- `opcom_kmsg_
classify()` decides facility from the TEXT PREFIX, not from tracing the
printk call site (which `/dev/kmsg` does not expose distinctly enough to
do cheaply). This is a deliberate, disclosed simplification: it does not
change the SEVERITY (still the kernel's real level) or the CONTENT (still
the kernel's real text), only which of two Rule-8 facility labels a line
wearing OVMX's own module-name prefix gets.

## 5. Mechanism: `/dev/kmsg`, not a new kernel channel

`/dev/vms`'s `file_operations` are ioctl + mmap only (`src/kernel/
vms_module.c`), and this item does **not** add a kernel-to-user push
channel -- that would be new kernel surface for a routing/formatting
concern, and INV-6 (no per-process executive fallback) has nothing to do
with this: nothing here fakes an executive facility, it surfaces real
kernel events that already happened.

Instead, `src/ovmx_init/opcom_kmsg.c` opens the **standard, pollable**
`/dev/kmsg` device:

1. `open("/dev/kmsg", O_RDONLY | O_NONBLOCK)`, then `lseek(fd, 0, SEEK_SET)`
   to the first still-buffered record -- so records `vms.ko`/`vmsfs.ko`
   already emitted before this reader started (module load happens early in
   `bare_metal_init()`) are replayed, not missed.
2. `poll()` + `read()` in a detached pthread: each `read()` returns exactly
   one kernel log record (`/dev/kmsg`'s documented per-record-per-read
   contract). The record's leading `<pri>,<seq>,<ts>,<flags>[,KEY=VAL]*;`
   header is parsed for the syslog priority; the text after `;` up to the
   first `\n` is the message.
3. `opcom_kmsg_classify()` (pure, unit-tested -- §3/§3b/§4 above) decides
   drop-or-route and formats the `%OVMX-<S>-<IDENT>, <text>` or
   `%SYSKRNL-<S>-KERNEL, <text>` line.
4. Every routed (`OPCOM_KMSG_OPERATOR_LOG`) line is appended to
   `SYS$MANAGER:OPERATOR.LOG` via `opcom_kmsg_append_operator_log()`.
   `/dev/console` is never opened by this file (§3b, round 3): the boot
   console's `%FACILITY-SEVERITY-IDENT` sequence is oracle-pinned
   (`tests/qemu/test_boot_conformance.sh`) and produced entirely by the
   boot orchestrator -- a kernel module's printk, reformatted or not, is
   never part of that narration.

Started from `bare_metal_init()` in `src/ovmx_init/ovmx_init.c`, right
before `executive_attach()` loads `vms.ko` -- early enough to catch the
module's own init-time lines. A failure to open `/dev/kmsg` is silently
non-fatal: this is a best-effort operator-visibility aid, not the executive
attach gate Rule 9 governs (that gate is `/dev/vms`, unchanged).

This is the seed of the aspirational detached OPCOM process noted in
`docs/design-init-scope.md` / `docs/design-boot-faithful.md` -- a real
OPCOM.EXE, run RUN/DETACHED like JOB_CONTROL, is future work (it would also
be where the (B)-vocabulary live-broadcast half of `sys$sndopr`/`REPLY
/ENABLE` eventually lives, once JOB_CONTROL-style detached-process
creation is extended to it). This item does not attempt that: it is a
lightweight bridge thread inside PID 1, deliberately scoped to the (A)
vocabulary this item's kernel-module messages actually are.

## 6. `sys$sndopr` / OPERATOR.LOG format fix

`src/libvms/syssvc/sys_operator.c` wrote (before this item):

```
%%OPCOM, DD-MON-YYYY HH:MM:SS.CC, request N from user USERNAME on node OVMX
<message text>
```

Two defects against the oracle (§2(B) above): the banner is one line with a
single `%%` pair and no boxing, and the node is a hardcoded literal `OVMX`
rather than the system's real SCSNODE.

**After this item:**

```
%%%%%%%%%%%  OPCOM  DD-MMM-YYYY HH:MM:SS.ss  %%%%%%%%%%%
Request N, from user USERNAME on NODE
<message text>
```

- The banner is built from an 11-`%` literal, not a `printf`-escaped
  format string, so the count cannot silently drift under a future edit
  that doesn't notice `%%` doubling.
- The month is rendered upper-case 3-letter (unchanged from before -- this
  was already correct) and the timestamp keeps its existing
  `DD-MON-YYYY HH:MM:SS.CC` layout, which already matches the oracle's
  `HH:MM:SS.ss` hundredths shape.
- `NODE` is read via `ovmx_node_name()` (`src/libvms/include/
  ovmx_identity.h`) -- the same SYSGEN-SCSNODE-with-`OVMX`-fallback accessor
  `SYI$_SCSNODE`, `F$GETSYI("SCSNODE")` and `SHOW SYSTEM`'s banner already
  use -- instead of the literal string `"OVMX"`. A clustered OVMX node now
  gets its own name in its own operator log, the way a real VMS node does.
- The `Request N, from user U on N` body-line-2 variant is **preserved**,
  not replaced with the bare `Message from user U on N` form: every
  `sys$sndopr` call in this tree already assigns a request number (the
  existing `req_count` counter), and the fix is to the banner/node shape
  the record wears, not to whether OVMX numbers its requests.
- `sys$sndopr`'s public signature is unchanged (Rule 4 of this item's
  brief) -- only what it writes to the log changed.

`tests/integration/test_opcom_record_body.sh` and `tests/qemu/
test_syssvc_ident.c` (`g_opcom_headers_naming`) previously scanned for the
old one-line `%%OPCOM, ...` header; both are updated to parse the new
two-line shape (banner line then `Request N, from user U on N`) while
asserting the exact same properties they always did (body carries the
caller's text with no control-byte corruption; the user field is the
executive's row, never a host-Linux or fabricated name).

## 7. What this item explicitly does not do

- No real OPCOM.EXE detached process (see §5's "seed" note).
- No live terminal broadcast for `REPLY/ENABLE`'d operators picking up
  kernel-module or SYSKRNL events -- only `sys$sndopr`/`sys$brkthruw`
  callers get that, unchanged from before this item.
- No change to `/dev/vms`'s kernel-side `file_operations` (still ioctl +
  mmap only) and no new kernel-to-user push channel.
- No per-subsystem `SYSKRNL` idents (one flat `KERNEL` ident covers every
  SYSKRNL line -- see §4).
- No severity-gated exception routing a critical (`level<=3`) SYSKRNL line
  to the console as a live broadcast. Round 2's operator correction offered
  this as an optional nice-to-have ("a genuinely critical SYSKRNL condition
  MAY broadcast to console like a real OPCOM operator broadcast"),
  explicitly marked optional with "keep it simple; when unsure, OPERATOR.LOG
  only" -- this item takes the simple branch: every routed line, any
  severity, any facility, goes to OPERATOR.LOG only (§3b round 3 made this
  even more absolute than round 2's own wording: OVMX-facility lines are
  included in "never the console" too, not just SYSKRNL).
  Superseded note: an EARLIER draft of this section said nothing would be
  captured to OPERATOR.LOG at all (mixing vocabulary (A) content into
  vocabulary (B)'s file was ruled out as a scope violation). Round 2
  reversed that for SYSKRNL lines, and round 3 (§3b) extended it to
  OVMX-facility kmsg lines too, once BOTH turned out not to belong on the
  console -- OPERATOR.LOG capture is how this bridge's information
  survives at all, now that none of it is console-routable.
