# GCC-as-VMS-Oracle — the production-compiler forcing-function lane (epic vms-da0)

**Status:** GO / feasibility ratified 2026-08-20. First-deliverable go/no-go.
**Lane:** peer conductor lane, parallel to 0.5, does NOT gate 0.5. Post-1.0 pole.
**Parent:** vms-df7 (OVMX self-hosts its own kernel — the ouroboros).
**Supersedes nothing; opens F1/F2 of** `docs/design-self-hosting-own-kernel.md`.

---

## 0. The governing constraint (operator, 2026-08-20)

> "The objective is to make OVMX **more VMS-faithful** while you build this GCC. GCC and the
> self-hosting toolchain are **the oracle, and OVMX must bend to it — not the other way around.**"

This is the load-bearing frame for the whole lane and it **decides the base pick**:

- The compiler is not a feature to be "gotten working." It is a **real VMS program** whose every
  OS-interface call — RMS file I/O, `LIB$SPAWN` subprocess pipeline, VMS condition handling,
  logical-name resolution, VMS file specs, the VMS calling standard — encodes what *real VMS
  provides*. When the compiler demands a facility OVMX fakes or lacks, that demand is a
  **VMS-faithfulness signal**, and OVMX is corrected **toward VMS** to satisfy it.
- **Bending direction is one-way.** We do **not** patch the compiler to use POSIX/musl to make it
  run on OVMX — that bends the oracle toward OVMX/Linux and reinforces the veneer (the exact
  INV-6 / LARP anti-pattern the authenticity program exists to kill). We patch **OVMX** to provide
  the genuine VMS facility the compiler expects, validated against the lab oracles
  (VAX 7.3 / Alpha 8.4) and public VMS documentation (Rule 8 clean-room).
- Therefore this lane is a **primary authenticity forcing function** (serves vms-898 authenticity,
  the executive-gap program, and the anti-cheat-flip audit): a real VMS toolchain only runs if
  OVMX's VMS facilities are *genuine*, not per-process fakes.

---

## 1. GO/NO-GO verdict: **GO**

Feasible, and the toolchain substrate to receive it already exists and is proven:

| Existing asset | State | Role for this lane |
|---|---|---|
| **TCC.EXE** | real vendored upstream TinyCC compiled as an OVMX-native image (`third-party/tcc/src/`, 3 `OVMX_RMS_IO` seams), emits ELF64 `ET_REL` | the **template**: a real GPL compiler activated as an OVMX image via IMGACT, I/O through RMS |
| **LINK.EXE** | consumes standard **ELF64 `ET_REL`** (`src/vmslink/link.c:287-297`), hardened for gcc `-fPIC` reloc vocabulary; emits OVMX symbol-vector `.EXE` (`.vms$sv`) | linker for OVMX-image toolchain programs — **no new object format needed** |
| **IMGACT.EXE / DECC$SHR** | PT_INTERP static-PIE activator; C RTL = whole-archived **musl** libc.a + libgcc.a | activation path + RTL floor (note: **no libstdc++** — a named backfill for C++ GCC) |
| **`/dev/vms` + `vms_kif`** | executive reached by client vector from LIBVMSSYS$SHR | the OS interface the oracle will exercise and force toward VMS-faithfulness |

Mechanical confirmations (this session):
- Modern GCC (musl, `-fPIC`) emits only `R_X86_64_{PC32,PLT32,REX_GOTPCRELX}` — **all in LINK.EXE's
  supported set**. Plain gcc `.o` already links today.
- GCC **always emits `.s` assembly text and invokes an external assembler**; it never writes ELF
  `.o` directly. tcc's integrated assembler is a GAS **subset** and cannot assemble GCC output.
  → **A full GNU `as` (binutils) is structurally required.** This is exactly F1 of the ouroboros doc.

---

## 2. Base pick: a **VMS-host GCC**, not a Linux GCC

The charter named three candidate bases. The operator's oracle constraint decides among them:

| Candidate | Verdict | Why |
|---|---|---|
| Stock Linux-musl GCC (my prior lean) | **REJECTED** | OS calls go musl→POSIX→Linux; touches **no** VMS facility; forces nothing VMS-authentic; would require shimming GCC toward OVMX — bends the oracle the wrong way |
| GNV "gcc" (VSI PCSI kit) | **RED FLAG — excluded** | GNV's "gcc" is **not** GCC — it is wrapper scripts around the **proprietary VSI/HPE DEC C** compiler giving it a gcc-compatible CLI. Not GCC's host layer hitting RMS/LIB$; it's DEC C's. Requires proprietary VSI source; defeats the forcing function |
| VSI OpenVMS x86-64 GCC | **RED FLAG — not viable** | No such GCC exists upstream or as obtainable source; VSI's x86-64 C/C++ are their own proprietary compilers (no source). No `x86_64-*-vms*` target exists in mainline GCC |
| Historical VAX/VMS GCC (gcc 2.8.1 / 3.x, `vax-vms`) | **archaeological oracle only** | GPLv2 FSF tarballs, legally obtainable; K&R-era, VAX target; keep as a VAX-target + VMS-behavior reference, not a modern base |
| **Upstream GNU binutils + mainline GCC, `alpha-dec-vms` as the VMS-host behavioral reference** | **PRIMARY BASE** | GPLv3+, actively maintained, freely obtainable (sourceware / bminor mirror); mainline GCC still configures **`alpha-dec-vms`** (the living VMS-host reference — `ia64-hp-vms` is being removed in GCC 15). No VMS artifact is vendored from VSI/GNV; **OVMX authors the x86_64/aarch64 VMS-host layer** from the `alpha-dec-vms` pattern |

**Configuration:** VMS-host × target. The **host layer is the forcing function** and is independent
of codegen target, so *first-light does not require a codegen retarget* — activating a VMS-host
toolchain image and watching which VMS facility it demands is already the signal. The x86_64 /
aarch64 **ELF** target (matching the existing OVMX runtime + DECC$SHR, and the kernel-build endgame's
GNU ld/BFD) is what the compiler's **output** uses; the VMS-host **input/self** layer is where the
RMS/LIB$ forcing happens.

### 2a. Scout findings (2026-08-20) — three corrections that sharpen the pick

A source-availability + licensing scout (clean-room: GPL/LGPL only, no VSI/HPE proprietary source)
resolved the exact base and surfaced a dilution risk that touches the operator's framing directly:

1. **Base = upstream, not a VSI/GNV artifact.** GNU binutils (GPLv3+) has long-maintained VMS
   backends (`bfd/vms-alpha.c`, `vms-lib.c`, `vms-misc.c`; gas `te-vms.*` + `obj-evax.c`;
   `tc-alpha.c` VMS hooks). Mainline GCC still configures `alpha-dec-vms` (VMS host+target headers:
   `gcc/config/vms/vms.h`, `xm-vms.h`, `ia64/vms.h`). **There is no `x86_64`/`aarch64` VMS target
   upstream** → OVMX authors that VMS-host support from the `alpha-dec-vms` behavioral pattern.
   Nothing is vendored from VSI/GNV. DEC C appears only as a *bootstrap* compiler in community
   VMS binutils builds (like MSVC bootstrapping GCC-on-Windows) — not incorporated, clean.

2. **The CRTL-Unix-shim dilution (load-bearing for the oracle framing).** Upstream GCC's *own*
   VMS host layer sets `__UNIX_FOPEN` / `__UNIX_FWRITE` / `_POSIX_EXIT` (`xm-vms.h`) — it
   deliberately asks the CRTL to behave Unix-stream-like rather than exercising native RMS FAB/NAM
   record semantics. So "build stock GCC-on-VMS as-is" would reinstate exactly the POSIX veneer the
   authenticity program exists to kill (INV-6), one layer below GNV's wrapper. **DECISION (lane
   authority — this is *how* to implement the operator's stated goal of making OVMX *more*
   VMS-faithful): OVMX drives the compiler's I/O to native RMS (FAB/NAM records); it does NOT honor
   the CRTL Unix-shim.** This is a deliberate OVMX design choice *beyond* upstream fidelity (logged
   as such, not attributed to upstream GCC), it keeps the forcing function strong, and it aligns
   with the ACP conductor's interest (a genuine RMS-over-ACP witness). Flagged to the operator as an
   FYI decision, not a blocker.

3. **`as` is validated as the strongest first image, and the object-format fork is resolved.**
   gas's VMS object writer (`obj-evax.c`) already works at real **FAB/NAM/DSC descriptor** level — a
   *stronger* RMS touchpoint than the GCC driver's generic host I/O. Resolution: **gas emits ELF**
   (LINK.EXE-consumable + kernel-endgame GNU-ld/BFD-compatible), configured **VMS-host** so its
   file I/O drives native RMS; `obj-evax.c` is the RMS-descriptor *reference* to borrow the
   discipline from, **not** the output writer (EVAX objects are a dead-end for the ELF kernel path).

**Genuine LIB$/condition-handling touchpoints confirmed as forcing targets** (from `ia64/vms.h`,
`libgcc/config/ia64/vms-unwind.h`): static constructors via `LIB$INITIALIZE#`; **VMS
condition-handling/unwind** (real dispatcher frames + nested handlers, *not* setjmp/longjmp);
`$GETSYIVER`-style CRTL/VMS **version identity** (`VMS_DEFAULT_CRTL_VER`). These are the OS facilities
the eventual GCC (F2) will force; `as` (F1) forces primarily RMS.

---

## 3. First forcing-function image + predicted first wall

Per the ouroboros sequencing (F1 binutils → F2 gcc), and because GCC structurally needs an
assembler, the **first buildable forcing-function image is GNU `as`** (binutils): plain C, smallest
real toolchain component whose ELF output LINK.EXE already consumes, and — built VMS-host — its file
I/O + BFD demand real VMS facilities immediately.

**Predicted first OS-facility walls** (to be confirmed by the F1 build+activate experiment, against
real `/dev/vms` under the tests/qemu KE harness — host/unit proofs are necessary but NOT sufficient):

1. **RMS record/file semantics** the assembler/BFD rely on (sequential + temp scratch files) — the
   most likely first wall, and squarely in the executive-gap / ACP territory the main conductor owns.
2. **`LIB$SPAWN` / subprocess pipeline** — the gcc driver spawns cpp→cc1→as→ld; VMS spawn semantics
   (not `vfork`/`execve`) over the executive.
3. **VMS condition handling** — the toolchain establishes handlers for its own errors (`$ESTABLISH`,
   signal arrays), not Unix `signal()`.
4. **Logical-name resolution + VMS file specs** — include/library search, temp-file naming.
5. **(F2, C++ GCC) libstdc++ / C++ runtime** — absent from DECC$SHR (musl+libgcc only) = named backfill.

Each wall → backfill **genuinely** in the shared executive/RTL/RMS/kernel-core → VAX (ILP32) +
Alpha (LP64) inherit → **3-way convergence gate applies** → the main/ACP conductor release-gates the
shared-core change. Loop until the VMS-host toolchain compiles real C in-guest.

---

## 4. Architecture fork (surfaced, non-blocking)

Per `design-self-hosting-own-kernel.md §4.2/4.4`, the **kernel-build output** (`vmlinux`) is native
ELF via an **in-guest GNU `ld`/BFD**, deliberately **bypassing LINK.EXE** (a kernel is not an OVMX
symbol-vector image). No conflict with the above: the **toolchain programs** (`as`, `gcc`, `cc1`)
are OVMX symbol-vector images (LINK.EXE-linked, like TCC.EXE); their **kernel output** is native
ELF. This means the lane grows a real in-guest GNU `ld`+BFD alongside LINK.EXE (F1). The endgame
shape (GNU ld/BFD for `vmlinux`) is the documented plan; flagged here for operator visibility.

---

## 5. Loop (the method, restated with the oracle frame)

```
pick/scout the VMS-host GCC upstream (F1 first: GNU as)
  → cross-build it VMS-host as an OVMX image (LINK.EXE, the TCC.EXE pattern)
    → ACTIVATE under IMGACT against real /dev/vms
      → it demands a VMS facility OVMX fakes/lacks   ← THE WALL = the oracle speaking
        → bend OVMX toward VMS: backfill the facility GENUINELY
             (public VMS docs + lab observation; NEVER a per-process fake; NEVER shim the compiler)
          → verify under the tests/qemu KE harness (real /dev/vms), 3-way convergence gate
            → release-gate the shared-core change through the main/ACP conductor
              → repeat
```

Endgame (post-1.0): GCC + kbuild build the **shipped kernel** in-guest → closes vms-df7.
