# Corpus Inventory Summary

Companion to `inventory.json` (machine-readable, 55 records) and the existing
`PROVENANCE.md` / `LICENSE-AUDIT.md`. Produced under `vms-e86`: extend the
2026-02-13 source-only research (`docs/vms-source-code-corpus.md`) with a
verified binary dimension, and acquire everything clearly redistributable.

Generated: 2026-08-04.

---

## Counts

**By kind:** source 45 · both (source+binary) 6 · binary 4

**By acquisition status:** committed 8 · fetch-script 42 · flagged 4 · skipped (dead link) 1

**By redistributable:** yes 28 · unclear 24 · no 3

**By arch (binary/both records only):** x86_64 2 · alpha 2 · ia64 1 ·
vax/alpha/ia64 (mixed) 4 · alpha/ia64/x86_64 (per-package) 1

---

## What's newly committed to git this session

| Dir | Source | License | Size |
|---|---|---|---|
| `tests/corpus/tier6-laxdriver/` | jhamby/vms-laxdriver | MIT | 66 KB |
| `tests/corpus/tier6-ipc-benchmark/` | jhamby/vms-ipc_benchmark | MIT | 112 KB |
| `tests/corpus/tier6-memtester/` | jhamby/vms-memtester | GPL-2.0 | 80 KB |
| `tests/corpus/tier6-cmatrix/` | jhamby/cmatrix | GPL-3.0 | 357 KB |

Total added to git working tree: **~615 KB** across 4 new source dirs (96
files). `PROVENANCE.md` and `LICENSE-AUDIT.md` updated in place with matching
entries. Nothing else was committed — everything else below is a fetch
script or a flagged lead.

These four were chosen over the many other jhamby ports because they are
small, carry an explicit OSI license, and — for `vms-laxdriver` and
`vms-ipc_benchmark` specifically — exercise VMS API surface nothing else in
the corpus touches (a real device driver; a full spread of IPC mechanisms).

---

## Top 10 acquisition targets, ranked by test value

1. **WASD x86-64 object-module kit** (`fetch/wasd.sh`) — real VSI-compiled
   x86-64 OpenVMS `.OBJ` code (226 modules, Apache-2.0, dated Jan 2026). The
   single most directly relevant binary find: same architecture and same
   VSI C compiler generation OVMX's `IMGACT.EXE` targets. Not yet a linked
   image — needs WASD's `INSTALL`/`UPDATE` DCL procedures run against a real
   or OVMX-hosted `LINK.EXE` before it's an activatable artifact — but it's
   the shortest path to one.
2. **WASD source kit** — prerequisite for #1, Apache-2.0, exercises ASTs,
   async `$QIO`, process control: the API surface the executive gap
   ([[executive-gap]]) is most under-tested against.
3. **vms-laxdriver** (committed) — the only corpus item that is a VMS
   *device driver*, not an application calling VMS APIs. Directly relevant
   to `vms.ko`/`vmsfs.ko` work.
4. **vms-ipc_benchmark** (committed) — nine IPC mechanisms in one small
   repo (pipes, FIFOs, UNIX sockets, TCP, UDP, POSIX MQ, shared memory,
   socketpair). Cheap, broad mailbox/shared-memory coverage.
5. **John Francis PCSI kits (AXP + IA64)** — the largest available body of
   *linked* Alpha/Itanium OpenVMS images with matching source. Lower
   priority than x86-64 per the task's own ranking, but the only volume
   source of real linked non-x86 images found.
6. **OpenSSL / curl upstream** — actively maintained, large, real-world
   crypto/networking VMS code; both already build for VMS today, so a
   compile-pass/fail signal against OVMX headers is meaningful immediately.
7. **plibsys** — cross-platform abstraction layer over VMS threads/sockets/
   IPC; useful precisely because VMS quirks are already documented in its
   wiki (named semaphores/shm broken on VMS 8.4+), giving a known-answer key.
8. **GNV (jhamby fork set, 13 repos)** — modern (2022-23), actively patched
   GNU userland ports; `gnv-bash` and `gnv-coreutils` are the deepest
   real-world exercises of process control + RMS file I/O in the entire
   lead list.
9. **vms-memtester / vms-ipc_benchmark siblings** (`vms-gmp`, `vms-libuv`) —
   further small-to-medium ports with clear licenses (GPL-2.0, MIT) and
   narrow, well-defined API surfaces good for isolating individual gaps.
10. **GNV SourceForge upstream kit** — the older, larger, PCSI-installable
    Alpha kit; redundant with #8 in coverage but the only one shipped as an
    actual installable package rather than raw source, useful once OVMX can
    process PCSI kits at all.

---

## Flagged for operator legal ruling

| Item | Why flagged |
|---|---|
| **VSI official PCSI ECO kits** (GNV V3.0-2F, VSI C++, PERF_UPD — genuine x86-64 binaries) | Gated behind the licensed `sp.vmssoftware.com` portal; VSI/HPE-proprietary. **Excluded outright** per Rule 8 / constraint 2 — recorded as a lead only, not fetched. |
| **OpenVMS Hobbyist/Community ISO images** (archive.org etc.) | Full OS distribution media. **Excluded outright** per constraint 2. |
| **VSI official freeware page** (`vmssoftware.com/community/freeware/`) | VSI disclaims ownership/support but the page does not itself grant redistribution rights; per-package terms not individually verified. Bison/Flex explicitly claim x86 support — worth a follow-up check for prebuilt x86-64 content. |
| **vmsbackup (FreddieAkeroyd) / VMSBackup (TonyBUK)** | Both repos show `license: null` on GitHub — no declared terms. Both are small; cheapest fix is an email to the authors. |
| **Digiater.nl Freeware CD mirror** | 600+ packages, license varies per package (mostly GPL per the original research, unverified per-package this pass). |
| **vms-ports SourceForge project** | Status shown as "Planning" with a suspicious future last-update timestamp; per-sub-project license unverified. |
| **Several jhamby ports** (`vms-regina`, `vms-coremark[-pro]`, `vms-dmpipe`, `vms-halls-of-zk`, `vms-glextrusion`, `vms-ramspeed-smp`, `vms-mesa-demos`, `vms-xscreensaver`, `vms-libjpeg-turbo`, `vms-perl5`, `vms-cpython`) | No root LICENSE file in the fork (GitHub reports NOASSERTION or null). `vms-coremark`/`vms-coremark-pro` specifically: upstream EEMBC CoreMark license restricts publishing benchmark results outside EEMBC's run rules — a real usage constraint, not just a missing-file technicality. |
| **Kermit for OpenVMS** | Kermit Project license is permissive but non-OSI with specific terms; not read closely this pass. |
| **NETLIB** (already in repo) | Pre-existing flag, unchanged — "All Rights Reserved," used read-only per standing policy. |

---

## Dead / stale links found

- **HP OpenVMS Example Programs** (`h41379.www4.hpe.com/opensource/cdsa_source.html`) —
  confirmed **HTTP 404** on 2026-08-04. HPE has decommissioned this page;
  no live replacement found. `SYS$EXAMPLES:`/`DECW$EXAMPLES:`/`TCPIP$EXAMPLES:`
  content ships only on an installed OpenVMS system, not as a standalone
  download — removing this as an actionable lead.
- All other links from `docs/vms-source-code-corpus.md` spot-checked this
  pass (Eight-Cubed, GitHub repos, digiater.nl, vsm.com.au/jfp, WASD,
  SourceForge GNV and vms-ports, process.com) **resolved live** as of
  2026-08-04.

---

## Surprises / notes for the next session

- **The x86-64 dimension is thinner than hoped.** OpenVMS x86-64 GA'd in
  2023; almost nothing beyond VSI's own gated kits and the single WASD
  object-module kit exists as freely downloadable x86-64 VMS binary content
  yet. Expect this to grow over time as the hobbyist/community license
  program matures — worth re-checking `vmssoftware.com/community/freeware/`
  periodically for x86-64 additions (Bison/Flex already claim x86 support;
  their actual zip contents weren't inspected this pass).
  See `tests/corpus/fetch/vsi-official-freeware.sh`.
- **jhamby's GitHub account is the single richest active lead**, not fully
  captured in the original 2026-02-13 research doc (which only mentioned
  "GNU ports by Jake Hamby, coreutils/diffutils/sed/make" in passing). It
  has ~30 `vms-*`/`gnv-*` repos, actively patched 2022-2023, none with a
  root LICENSE file except a handful (MIT/GPL-2.0/BSL-1.0) — a systematic
  license request to the maintainer would unlock a lot of otherwise-usable
  source at once.
- **PCSI kits are the practical bridge to linked Alpha/Itanium binaries.**
  No other lead in this pass produced *linked, runnable* non-x86 VMS images
  in bulk; John Francis's collection is the whole of that story.
