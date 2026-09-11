# R6 — Vendored / Bundled Dependency License Audit

**Gate:** R6 (V1.0 release-engineering). OVMX is **given away free and redistributed** (bootable image `ovmx-distrib.img`, kit `ovmx-os.kit`), so every vendored/bundled component that **ships** must be cleared for redistribution under that model, with its license obligations satisfied.

**Scope:** the vendored/bundled dependencies that ship in — or are linked into — the redistributed artifacts. Distinct from two orthogonal, already-covered tracks: `docs/clean-room/PROVENANCE.md` (clean-room provenance of OVMX's *reimplemented* surfaces) and `tests/corpus/LICENSE-AUDIT.md` (the *ingested test corpus*, not redistributed). rd vms-251.

**Grounded on:** `origin/main` @ `59d582c7`.

**Nature of this document:** a **technical inventory + redistribution-compatibility assessment with flags** — **NOT legal advice**. It exists so Baron (with counsel as needed) can make the R6 clearance ruling; it produces the evidence, it does not make the legal determination.

---

## 1. OVMX's own base license

`LICENSE` is the verbatim **GNU GPL version 2** (FSF text); `README.md:114` labels it "GPL-2.0"; OVMX's own kernel sources carry `SPDX-License-Identifier: GPL-2.0`. No "or later" grant is present → effectively **GPL-2.0-only**.

**Compatibility framing:** for the give-it-away model, every bundled/shipped dep must be **GPL-2.0-compatible**. Every shipped dep below is: **LGPL-2.1, MIT, and BSD/ISC all combine cleanly with GPL-2.0**, and the Linux kernel *is* GPL-2.0. **There is no license-INCOMPATIBILITY finding.** The findings are **obligation-satisfaction** ones (license texts, notices, source offer) — see §4.

---

## 2. The discriminator: SHIPPED vs LINKED vs BUILD-ONLY

Redistribution obligations attach only to what is **redistributed**. A **build-only** tool (used to build, not present in the image) carries no redistribution obligation; a component **shipped in the image** or **linked into a shipped binary** does. Per-dep classification (the crux):

| Dep | License / SPDX | Version | Incorporation | Modified? | Redistribution obligation |
|---|---|---|---|---|---|
| **tinycc** (`TCC.EXE`) | **LGPL-2.1** (`third-party/tcc/src/COPYING`; RELICENSING-to-MIT **incomplete** — arm-gen.c=NO → stays LGPL) | mob `85ba3ae8` (0.9.28rc), 2026-07-24 | **SHIPPED** — staged into `SYSEXE` on `ovmx-distrib.img` + `ovmx-os.kit` (Dockerfile.bootable:495,665,812). Static-musl binary, **unmodified** upstream (the `ovmx/` shim is a *separate* build, not the shipped TCC.EXE) | No (shipped binary) | **COPYLEFT.** Ship LGPL-2.1 text + copyright notices; corresponding source (present in-repo at `third-party/tcc/src`); static link → LGPL §6 relink/source offer |
| **Linux kernel** (`vmlinuz`) | **GPL-2.0-only** | 6.12.103 LTS, SHA-pinned from kernel.org (Dockerfile.bootable:157) | **SHIPPED** — `vmlinuz` in the bootable image (Dockerfile.bootable:298,1221). Kernel proper unmodified; OVMX drivers overlaid in-tree | Drivers overlaid (own code) | **COPYLEFT.** Written offer / corresponding source for the exact pinned tarball; license text |
| **vms.ko** (OVMX module) | **GPL-2.0** — `MODULE_LICENSE("GPL")` + SPDX (`src/kernel/vms_module.c:1,46`) | OVMX own | **SHIPPED** — initramfs `/lib/modules/vms.ko` + kit (Dockerfile.bootable:1112) | OVMX's own code | No taint concern — GPL, in-tree (`intree=Y`, no `TAINT_OUT_OF_TREE`). Covered by OVMX's own GPL-2.0 |
| **musl** | **MIT** | `alpine:3.20` `apk add musl-dev` (Dockerfile.bootable:53) | **SHIPPED (linked)** — statically linked into **every** shipped `.EXE` (DCL, LOGINOUT, STARTUP, TCC, LINK, MMK, SYSGEN, …) | No | Include MIT text + copyright notice (permissive) |
| **OpenSSH** | BSD-2 / ISC / public-domain (tree `LICENCE`) | 10.0p1 pin (content self-IDs 10.0p2) | **BUILD/CI-ONLY — NOT shipped on main** (only CI `openssh-static-musl`, ci.yml:156; the SSH rung is not live-wired into image/kit) | `ovmx/` veneer + veneer patches | Permissive; obligation attaches **only when it ships** (→ gate vms-751) |
| **LibreSSL** (`libcrypto`) | **ISC + dual OpenSSL/SSLeay** (tree `COPYING`) | 4.1.2, SHA-pinned | **BUILD/CI-ONLY — NOT shipped on main** (only CI `libcrypto-static-musl`, ci.yml:153) | Unmodified (zero patches) | Permissive/dual; obligation **only when it ships** (→ gate vms-751) |
| **GCC** (cross alpha/vax) | GPL-3.0-or-later + Runtime Library Exception | `tools/cross-alpha-vms/gcc-14.2.0.tar.xz` (committed) | **BUILD-ONLY** cross-compiler; output not in x86_64 image. But the **pristine source tarball is committed** → redistributed as source in the git repo | No (pristine) | Copyleft source obligation **self-satisfied** (pristine upstream source is what's redistributed). Flag as bundled source |
| **binutils** (cross) | GPL-3.0-or-later | `tools/cross-vax/binutils-2.42.tar.xz`, `tools/cross-alpha-vms/binutils-2.43.tar.xz` (committed) | **BUILD-ONLY**; pristine source tarballs committed/redistributed in-repo | No | Same as GCC — self-satisfied |
| **musl** (cross src) | MIT | `tools/cross-alpha-vms/musl-arch/musl-1.2.5.tar.gz` (committed) | **BUILD-ONLY** cross-build source | No | Permissive |

---

## 3. Copyleft flags — the real gate

**SHIPPED copyleft in the redistributed image/kit** (the obligations that actually bite):
1. **tinycc LGPL-2.1** — as `TCC.EXE` (the self-hosting bootstrap compiler). **Confirmed it ships.** The RELICENSING effort is incomplete, so it is LGPL, not MIT.
2. **Linux kernel GPL-2.0** — as `vmlinuz`.
3. **vms.ko GPL-2.0** — OVMX's own, covered by the base license.

**SHIPPED permissive:** **musl MIT**, statically linked into every shipped `.EXE`.

**Not shipped (lower concern, gated):** OpenSSH + LibreSSL are permissive and **CI-build-only on main** — the SSH rung is not live-wired into the image/kit. Their notice obligations attach *when they ship* (→ gate vms-751).

**Bundled source only (self-satisfied):** GCC/binutils GPL-3.0 cross-toolchains are committed as **pristine upstream source**, build-only — the source-redistribution obligation is met by the pristine tree itself.

**No incompatibility exists** — OVMX's GPL-2.0 base combines cleanly with all of them.

---

## 4. THE compliance gap (concrete, actionable) → rd vms-72c7

**The redistributed artifacts carry NO license texts, NOTICE, or written source offer.** `git ls-tree` finds no `COPYING`/`LICENSE`/`NOTICE` under `distro/rootfs/`, the initramfs, or the kit staging. The bootable image ships **LGPL-2.1 tinycc + MIT musl (in every EXE) + GPL-2.0 kernel + GPL-2.0 vms.ko** but **bundles none of their license texts or copyright notices, and offers no corresponding source.**

Under the give-it-away redistribution model, the minimum to satisfy the obligations of the currently-shipped deps is to **ship, alongside `ovmx-distrib.img` / `ovmx-os.kit`:**
- **GPL-2.0** text (covers the kernel, vms.ko, and OVMX's own code),
- **LGPL-2.1** text + tinycc's copyright notices (for the shipped `TCC.EXE`),
- **MIT** text + musl's copyright notice (linked into every `.EXE`),
- a **written offer for corresponding source** (or a URL to it) for the GPL kernel and the LGPL tinycc — satisfiable since OVMX is open and the tinycc source is in-repo, but **the offer/pointer must physically accompany the binary artifacts**.

A conventional form is a top-level `THIRD-PARTY-NOTICES` / `licenses/` directory staged into the image + kit, plus a line in the release notes pointing to the corresponding-source repo.

**Gated follow-on (vms-751):** before OpenSSH/LibreSSL ship (the SSH rung), add their BSD/ISC + OpenSSL/SSLeay notices to the same bundle.

---

## 5. Redistribution-compatibility assessment (input to Baron's R6 clearance — his/counsel's reserved call)

- **License compatibility: CLEAR.** OVMX is GPL-2.0-only; every shipped dep (LGPL-2.1 tinycc, MIT musl, GPL-2.0 kernel/module) is GPL-2.0-compatible. Nothing shipped is license-incompatible with the give-it-away model, and nothing shipped is proprietary/no-redistribution.
- **Obligation satisfaction: ONE GAP (vms-72c7).** The shipped copyleft + permissive deps' license texts, notices, and source offer are **not currently bundled** with the redistributed artifacts. This is a real, mechanical 1.0-publication compliance gap — **not a licensing conflict, a packaging omission** — and it is straightforward to close (stage the texts + a source offer into the image/kit).
- **The sharp item (tinycc LGPL):** because `TCC.EXE` ships and the RELICENSing-to-MIT is incomplete, LGPL-2.1 applies to the redistribution — its text + notices + corresponding-source offer must be present. Closing vms-72c7 covers it.
- **Recommendation:** OVMX can ship 1.0 under its GPL-2.0 give-it-away model **cleanly**, conditional on closing **vms-72c7** (bundle the license texts + NOTICE + source offer) before publication, and honoring the **vms-751** gate before OpenSSH/LibreSSL ship. No dep needs removal or relicensing; no incompatibility exists.

---

## 6. Filed findings (report-not-fix)

| rd | Finding | Severity | Gate |
|---|---|---|---|
| vms-72c7 | Redistributed image/kit ships GPL/LGPL/MIT deps but bundles no license texts / NOTICE / written source offer | **1.0-publication** | before publication |
| vms-751 | Before OpenSSH/LibreSSL ship (SSH rung), bundle their BSD/ISC + OpenSSL/SSLeay notices | gated | before SSH ships |
