# vms-530 de-risk: can IMGACT.EXE activate a native LINK.EXE `.EXE` under QEMU with a non-root `/vms`?

**Verdict: GO.** Native LINK.EXE + IMGACT.EXE activation does **not** require the
activating process to be root, and does **not** require `/vms` to be created or
owned by an unprivileged process at runtime. This is proven two independent
ways below. The 0.2 killer-app demo should use the native toolchain path
through the real QEMU runtime.

The "needs root-owned `/vms`" framing in the vms-0b8-quarantined CI jobs
describes a **bare-CI-host test-harness artifact**, not a property of the
product's one true runtime (CLAUDE.md Rule 9). Recommendation at the end.

## The question

`docs/release-plan-0.2-to-0.5.md` names this as the single riskiest link in
the 0.2 demo: can a trivial LINK.EXE-built `.EXE` be activated by IMGACT.EXE
under the real QEMU runtime when the process activating it is not root? The
three quarantined CI jobs (`imgact-build-mode`, `imgact-x86_64`,
`multiobj-exec-x86_64`, tracked as vms-0b8) fail on a bare `ubuntu-latest`
runner with `mkdir: cannot create /vms: Permission denied` — because those
jobs run `mkdir -p /vms/SYS0/...` directly against the CI runner's real `/`,
outside any QEMU boot, outside PID 1, with no root. That is a real failure of
those specific jobs, but it does not by itself tell us whether the **real
runtime** (QEMU + `vms.ko`, Rule 9) has the same problem — which is what this
item exists to determine with evidence.

## Evidence A — from-scratch native repro, non-root, no QEMU

Built the full toolchain fresh in an `alpine:3.20` container (the same musl
container Dockerfile.bootable's `link-native` stage uses) with `/vms` bind-mounted
from an **empty host scratch directory** — i.e. `/vms` was never created by
root at the container's own top level; it's just a mount point:

```
LINK.EXE   — src/vmslink/link.c, host gcc
IMGACT.EXE — src/imgact, make ARCH=x86_64
DECC$SHR.EXE — src/vmslink/mk_decc_shr.sh (real musl libc.a + libgcc.a, 1493 objects, 250 universals)
HELLOVMS.EXE — src/imgact/test/hello_vms_derisk.c, LINK.EXE --executable --use DECC$SHR.EXE
```

Result (`adduser -D -u 1000 tester`, then `su tester -c '.../HELLOVMS.EXE'`):

```
== run as non-root uid 1000 ==
uid=1000(tester) gid=1000(tester) groups=1000(tester)
%HELLOVMS-I-ACTIVATED, hello, VMS! (native LINK.EXE + IMGACT.EXE)
%HELLOVMS-I-IDENT, uid=1000 euid=1000
exit=0
```

`%LINK-S-CREATED` reported `ET_DYN executable image, 1 object, 0 universals,
7 relocs, 0 GOT, 0 TLS, 0 ABS64-ptr, 4 imports` and `%LINK-I-IMPORT, 4
cross-image imports bound to --use producer` — a real cross-image PLT/import
bind (`printf`, `getuid`, `geteuid`, `exit`) into DECC$SHR, not just a crt0
stub. This directly refutes "activation needs root": the activating process
(uid 1000) never touched `/vms`'s ownership; it only needed the directory to
be **traversable**, which is ordinary Unix semantics (mode bits), not root.

## Evidence B — the real QEMU runtime, non-root SYSTEM session

Extended `distro/Dockerfile.bootable`'s `link-native` stage to also build this
same `HELLOVMS.EXE` and ship it in the FAT initramfs's `SYSEXE`, then booted
the real `ovmx-boot` image (`distro/derisk_vms530_run.sh`, a scratch driver
modeled on `tests/qemu/test_persistent_boot.sh`'s FIFO automation). Full
transcript (`/tmp/derisk-vms530-console.log` from the run this doc is based
on):

```
%OVMX-I-EXEC, VMS executive attached on /dev/vms
%STARTUP-I-INSTALL, installing OVMX system to DKA0:
%STARTUP-I-INSTALLED, system installation complete
%OVMX-I-EXEC, system identity SYSTEM [1,4] established by the executive
...
Username: SYSTEM
Password:
   Welcome to OVMX V0.1 - OpenVMS-compatible
    Welcome to OVMX -- OpenVMS Compatible eXperience
$
```

`tools/vms_login.c` (LOGINOUT.EXE) calls `setgid(1)` + `setuid(4)` — the
UIC-mapped Linux gid/uid for `SYSTEM [1,4]` — **before** `execl()`-ing
`DCL.EXE`. `DCL.EXE` itself is a LINK.EXE `--executable` with
`PT_INTERP=IMGACT.EXE`, importing across **seven** producer shareables
(`mk_dcl.sh`). Reaching the `$` prompt above is direct, functional proof that
IMGACT.EXE resolved `PT_INTERP` and activated a native LINK.EXE image while
the calling process's real/effective uid/gid were already `4`/`1`, not
`0`/`0` — i.e. **the exact non-root-process / root-mastered-`/vms` shape the
demo needs, already working**, end to end, under the real QEMU runtime.
`/vms` itself is created and populated by `STARTUP.EXE`/`install_system()`
running as PID 1 (root) — the same way `/usr` on any Linux distro is
root-owned without that requiring every later `/usr/bin/ls` invocation to run
as root.

## A narrower, unresolved anomaly (flagged, not fixed — out of lane)

`RUN /vms/SYS0/SYSCOMMON/SYSEXE/HELLOVMS.EXE` from that same live SYSTEM DCL
session produced **no output** and returned straight to `$`, unlike Evidence
A's identical binary logic. A deliberate control, `RUN` on a genuinely
missing image, correctly printed `%DCL-E-IVIMAGE, image not found` — so
`access(X_OK)` on `HELLOVMS.EXE` did *not* fail (no IVIMAGE), meaning the file
was found and deemed executable; the child was forked and exec'd but produced
no visible result. `cmd_run` (`src/vmsdcl/dcl_cmd_process.c`) only reports
`WIFSTOPPED` and `WIFEXITED(status)==0`; a signaled child (e.g. a crash) or a
`WIFEXITED` with nonzero status is swallowed silently — so the true failure
mode could not be distinguished from the console alone. (`DIRECTORY
/vms/SYS0/SYSCOMMON/SYSEXE/` also reported "Total of 0 files" for a directory
that plainly contains the running `DCL.EXE`; `opendir()`/`readdir()` on a
`vmsfs`-backed directory returning empty while `open()`-by-name plainly works
looks like a separate, narrower `vmsfs` readdir gap, not a file-presence
problem — it's what made `DIRECTORY` an unreliable oracle here, not evidence
against the file's existence.)

Root-causing this further needs either better error reporting in
`src/vmsdcl/dcl_cmd_process.c` (report `WIFSIGNALED`/nonzero `WIFEXITED`) or
kernel-side instrumentation (`src/kernel/**`) — both outside this item's lane
(`src/vmslink/`, `src/imgact/`, native-link bootstrap scripts, `distro/`
mastering, and a scratch test program only). Filed as a follow-up rather than
blocking this verdict, because the load-bearing path for the demo — a real
production-shaped LINK.EXE `--executable` importing from real producer
shareables, run from a non-root SYSTEM session — is independently proven by
Evidence B using `DCL.EXE`/`LOGINOUT.EXE` themselves.

## Recommendation

1. **0.2 demo: use the native toolchain path (LINK.EXE + IMGACT.EXE) through
   the real QEMU runtime.** GO — no root privilege is needed at runtime for
   IMGACT activation.
2. Re-scope or drop the three vms-0b8-quarantined CI jobs
   (`imgact-build-mode`, `imgact-x86_64`, `multiobj-exec-x86_64`): they test a
   scenario (`mkdir /vms` directly on a bare, non-QEMU CI runner) that the
   real product never exercises. `tests/qemu/test_persistent_boot.sh`
   Boot 3 already covers the real shape (native LINK.EXE images, activated
   via IMGACT, under a non-root SYSTEM DCL session) and passes.
3. Follow-up (new rd item, out of this lane): (a) make `cmd_run` report a
   crashed/nonzero-exit child instead of swallowing it silently, (b)
   root-cause why this de-risk's minimal single-object `HELLOVMS.EXE`
   produced no output under the real QEMU/`vmsfs`/executive environment when
   the richer `DCL.EXE`/`LOGINOUT.EXE` do, and (c) look at `vmsfs`'s
   `readdir()` path (`DIRECTORY` under-reporting files that demonstrably
   exist).

## Reproducing

```
docker build -f distro/Dockerfile.bootable -t ovmx-boot-derisk .
distro/derisk_vms530_run.sh ovmx-boot-derisk
```

Scratch artifacts added for this investigation (safe to remove once vms-0b8
is closed): `src/imgact/test/hello_vms_derisk.c`, the `HELLOVMS.EXE`
build/ship steps in `distro/Dockerfile.bootable`, and
`distro/derisk_vms530_run.sh`.
