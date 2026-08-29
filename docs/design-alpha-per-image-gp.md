# Design: Authentic OpenVMS-Alpha Per-Image GP Establishment (D0)

**Status:** design, conductor-gated (2026-08-29). Blocks all implementation components of vms-5f5.
**Scope:** the callee-side global-pointer / linkage-section addressability model for OVMX
alpha-dec-vms code, so cross-image calls into multi-procedure shareables (DECC$SHR, LIBOTS)
stop skewing R27-relative static loads. This is the root cause of the crtl_rms N=7 blocker.

> **Clean-room provenance (Rule 8).** Every VMS-behavioral claim in this document is cited to a
> PUBLIC OpenVMS/VSI manual (the OpenVMS Calling Standard, the MACRO Compiler Porting and User's
> Guide, the Linker Utility Manual). Where the public docs do **not** publish a byte-level layout —
> notably the on-the-wire encoding of the GP-establishment relocation — OVMX defines its **own**
> representation and this document **LABELS it as an OVMX design choice**, never as VMS-authentic.
> No VSI/HPE source or binary was disassembled, decompiled, or consulted.

---

## 1. The defect (grounded)

### 1.1 What the port's cc1 does today
The alpha-dec-vms GCC backend emits `.base $27` — it addresses each compilation unit's linkage-section
data (external addresses, large constants, linkage pairs) **relative to R27**, with no `ldgp`. This is
confirmed by the backend: `gcc/config/alpha/alpha.cc:7773-7777` emits `gen_prologue_ldgp()` **only** for
`TARGET_ABI_OSF`; the `TARGET_ABI_OPEN_VMS` path (`alpha.cc:7767-7768`) sets only `reg_offset` and
establishes no GP. So on the VMS ABI the "GP" is simply whatever R27 holds on entry.

### 1.2 What R27 holds on entry (grounded)
Per the **VSI OpenVMS Calling Standard §3.3**, "all procedure values are defined to be the address of
the data structure (a procedure descriptor) that describes that procedure," and **§3.1.1** passes that
procedure value in **R27** (PV). The OVMX transfer path is faithful to this:
`src/imgact/arch/alpha/vms_transfer.S:61-63` loads R27 = PV (the PDSC address), fetches the entry code
address from `*(PV+8)` (PDSC$Q_ENTRY), and does a standard JSR — it does **not** load the callee's GP,
because "the callee owns its GP" per the calling standard (the trampoline reloads only *its own* GP after
return, `vms_transfer.S:64`). So **R27 = &PDSC on entry, and nothing external establishes the callee's
linkage-section addressability** — the callee must do it.

### 1.3 Why `.base $27` is wrong for multi-procedure images (grounded)
Per the **MACRO Compiler Porting and User's Guide §2.3.1**, a **linkage section** is "a program section
(psect) containing: Addresses of external variables, Constants too large to fit directly in the code
stream, Linkage pairs, **Procedure descriptors**." A **linkage pair** "contains the address of the
callee's procedure descriptor, and the entry point address" and "resides in the caller's linkage
section"; and "each routine has one unique procedure descriptor, and it is located in its linkage
section." On Alpha, "all external (out-of-module) references are made through a linkage section."

The load-bearing consequence: **one linkage section contains _many_ procedure descriptors** (one per
routine in the module) plus that module's linkage pairs and external-address cells. Therefore the natural
addressing anchor for the linkage section is the **per-module linkage-section base**, which is a single
address for the whole image — **not** any one procedure's PDSC address.

The port anchors R27-relative loads on R27 = &PDSC (the *called* procedure's descriptor). That equals the
linkage-section base **only** when the module has exactly one procedure whose PDSC sits at the section base
(the single-procedure, N≤3 minimal-image case — which is exactly why N=3 activates and larger images skew).
When a shareable (DECC$SHR: hundreds of routines) is entered at procedure *B*, R27 = &PDSC_B, an address
some non-zero offset K_B into the linkage section; every `K(R27)` load in *B*'s body that the compiler
computed as an offset from the *section base* now lands K_B bytes off. That is the skew that corrupts the
DECC$SHR CRTL round-trip at N=7.

**Measure-first note.** The vms-5f5 epic sketched the fix as "displacement = module_GP − &PDSC via the
EVAX format's OP_PUSH/OP_PSUB/OP_STORE relocation stack." The grounding sweep refuted the *mechanism*:
`src/vmslink/evax_read.c` is a read-side **recognizer** only — there is no `OP_PSUB`/subtract opcode
(`OPR_ADD`=101 is a no-op stub, `evax_read.c:299`), no apply path, and `link.c` is the ELF→OVMX
shareable *producer*, not an EVAX writer (`evax_add_ximport`/`evax_find_sym`/a "quad[1]=&PDSC" emitter do
not exist there). The epic's *model* — a per-module GP established as a link-time displacement from
R27=&PDSC — is correct and is what §1.4/§2 below specify; the *encoding* is redefined accordingly.

### 1.4 The authentic model
Because the linkage section is the per-module addressability anchor (§2.3.1) and each PDSC lives at a
fixed, link-time-known offset within it, the callee's linkage-section base ("module-GP") is:

```
module_GP  =  &PDSC  −  K
```

where **K = the offset of _this procedure's_ PDSC from its module's linkage-section base**, a value known
only at link time (only the linker has laid out the final linkage section). For a single-procedure module
whose PDSC is the first thing in the linkage section, **K = 0 ⇒ module_GP = R27**, and the current
`.base $27` behavior is already correct — this is the property the API-compatibility gate (vms-8208)
asserts as a real activation, not a link check.

---

## 2. OVMX representation (LABELED design choices, Rule 8)

The public docs establish the *model* (§1). They do **not** publish the byte-level instruction sequence or
relocation encoding by which a callee derives its GP (OSF/Alpha uses `ldgp`/`GPDISP`, but **EVAX has no
GPDISP relocation** — the OVMX EVAX recognizer table `evax_read.h:85-91` has REFLONG/REFQUAD/CODEADDR/
LINKAGE/NOP/BSR/LDA/BOH and no GP-displacement form). The following are therefore **OVMX design choices,
labeled as such, not presented as VMS-authentic:**

### 2.1 [OVMX] The module-GP register and prologue sequence
OVMX keeps the port's existing decision to address the linkage section R27-relative, and **re-bases** it:
the `TARGET_ABI_OPEN_VMS` prologue computes `module_GP` into the register the body already treats as the
linkage base, via an `ldah`/`lda` immediate pair carrying the signed constant `−K`:

```
;  [OVMX] GP-establish prologue, TARGET_ABI_OPEN_VMS
;  on entry R27 = &PDSC (VMS Calling Standard §3.3)
ldah  <gpreg>, HIGH(-K)(R27)      ; [OVMX] high half of the link-time displacement
lda   <gpreg>, LOW(-K)(<gpreg>)   ; [OVMX] low half; <gpreg> now = module_GP = &PDSC - K
```

`<gpreg>` and whether it is R27 rebased-in-place or a distinct reserved register is an **OVMX choice**
(the body's `K(gpreg)` loads are emitted against the same register). When `K` fits in the `lda` 16-bit
signed range the `ldah` collapses to a no-op; when `K = 0` the whole sequence is a no-op-equivalent
(§1.4 single-proc cascade).

#### 2.1.1 [OVMX] `<gpreg>` = **$15**, a labeled OVMX divergence (Rule 8) — resolved in C3 (vms-095)

C3 resolves `<gpreg>` to the reserved register **$15**, established by measurement against the actual
`alpha-dec-vms` backend (not `alpha.h` in isolation):

- **Authentic OpenVMS-Alpha uses R29 as GP.** On this GCC port, however, `gcc/config/alpha/vms.h`
  `#undef`s and redefines **`HARD_FRAME_POINTER_REGNUM = 29`** (overriding `alpha.h`'s 15), and the VMS
  prologue actively uses **$29 as the frame pointer** (`mov $30,$29`, `.frame $29`, saved/restored) in
  every stack procedure — i.e. exactly the multi-procedure DECC$SHR functions this design targets. So
  **$29 is not a free GP here**, and rebasing it, or establishing module_GP into it, corrupts the frame
  pointer. (An earlier reading that took `alpha.h`'s `HARD_FRAME_POINTER_REGNUM = 15` at face value and
  concluded "$29 is free" is refuted by the `vms.h` override; verified on emitted code.)
- **R27 rebased-in-place does not survive calls.** On the VMS call path R27 (PV) is reloaded to the
  callee's PV before each `jsr` and to the saved `&PDSC` after it, so a module_GP parked in R27 is
  clobbered by every call.
- **$15 is the free, call-saved register.** On this target `$2–$15` are callee-saved and `$15` is not the
  frame pointer (the `vms.h` register-role comment "$15 (frame pointer)" is a stale OSF copy-over).
  C3 reserves `$15` (`FIXED_REGISTERS[15] = 1`) as the OVMX module-GP and addresses the linkage section
  through it (`.base $15`). **This `$15`-as-GP choice is an OVMX codegen divergence, labeled as such
  (Rule 8): it is not VMS-authentic, and it is safe precisely because there is no VSI-binary interop —
  every OVMX-built shareable uses `$15` consistently.**

**No per-call GP reload is needed — and this is now general, not accidental.** Because `$15` is
callee-saved, a callee that establishes its own module_GP first **saves the caller's `$15` on entry and
restores it before `RET`** (C3 adds `$15` to the procedure's save mask). Therefore the caller's module_GP
survives every call — intra-module, cross-module-intra-image (where caller and callee have *different*
module_GPs), and cross-image alike. The earlier "no per-call reload" reasoning, written when `<gpreg>`
was imagined as `$29`, was only accidentally true for same-image/same-linkage-section calls; with a
reserved callee-saved `$15` plus mandatory save/restore it is **actually** true and general. The
save/restore is load-bearing: omit it and a cross-module return resumes the caller with the callee's
module_GP → wrong linkage base → silent corruption (this is the property C3's test asserts by objdumping
both the `$15` save and the `$15` restore).

### 2.2 [OVMX] The GP-establishment relocation
The two immediates above are **not** known to the compiler — only the OVMX linker knows each PDSC's final
`K`. So gas emits, on the `ldah`/`lda` pair, an **OVMX-defined relocation** that the OVMX linker resolves
by patching `−K` (split high/low) into the pair. Named here **`EVAX_R_OVMX_GPDISP`** (`[OVMX]` — this is
OVMX's analogue of OSF `GPDISP`, with an OVMX wire encoding because EVAX publishes none):

- **[OVMX] wire form:** carried as a store-class ETIR command consistent with the existing framing
  (`evax_read.c:278-282`: `cmd[2] + length[2]` header, length-includes-header, then operand). The operand
  identifies the two patch sites (the `ldah` and the `lda`, e.g. by their section-relative vaddrs or as a
  vaddr + fixed +4 delta) and the target symbol whose value is `K` (the PDSC's linkage-section offset).
  Exact operand byte layout is specified in C2 (vms-4ed) and **labeled OVMX** in this doc's cascade update.
- **[OVMX] resolution:** at OVMX link time the linker computes `K` from the assembled linkage-section
  layout and stores `−K` split across the `ldah` (high 16, with the +0x8000 carry adjustment standard for
  a signed `ldah`/`lda` pair) and `lda` (low 16) immediates. This is a *link-time* patch of a final image,
  so — unlike OSF GPDISP — it need not survive as a runtime relocation in the produced OVMX shareable.
- **[OVMX] recognizer + apply:** `evax_read.c`'s ETIR dispatch (`switch(cmd)`, `evax_read.c:286`) gains the
  new command (an unknown ETIR command is a hard error today, `evax_read.c:358-359`, so the recognizer
  must be taught it); the apply/patch step is added on the OVMX link side (C1/C2).

### 2.3 [OVMX] Linkage-section base recorded per image
The OVMX shareable producer (link.c) must define and record the per-image linkage-section base and each
procedure's `K`, so `EVAX_R_OVMX_GPDISP` can be resolved. This is C1 (vms-fd5). The DECC$SHR build
(mk_decc_shr.sh, C4/vms-8f4) exports the same so a cross-image callee resolves against the shareable's own
linkage-section base.

---

## 3. What stays untouched (invariant)

- **IMGACT cross-image linkage pair is unchanged.** The `{entry = *(PV+8), PV = &PDSC}` contract in
  `src/imgact/imgact.c` (imgact_fill_import) and the transfer in `vms_transfer.S` are the authentic
  Calling-Standard linkage pair (§2.3.1 "the address of the callee's procedure descriptor, and the entry
  point address"). This design changes **only** how a callee, once entered with R27=&PDSC, establishes its
  own linkage-section addressability. **quad[1]=&PDSC stays as-is.**
- The single-procedure / minimal-image activation path (K=0) must remain a no-op-equivalent — asserted as
  a real activation by the API-compat gate (vms-8208), never weakened to a link-only check (INV-6, Rule 9).

---

## 4. Component map (how D0 grounds C1–C4 + the gates)

| Comp | Item | D0 grounding |
|------|------|--------------|
| C1 | vms-fd5 | §2.3 — link.c computes the per-image linkage-section base + each PDSC's K |
| C2 | vms-4ed | §2.2 — the `[OVMX] EVAX_R_OVMX_GPDISP` reloc: gas emit + evax_read.c recognizer + OVMX-link apply |
| C3 | vms-095 | §2.1 — the `TARGET_ABI_OPEN_VMS` GP-establish prologue (ldah/lda, module_GP=&PDSC−K) |
| C4 | vms-8f4 | §2.3 — mk_decc_shr.sh lays out + exports the shareable's linkage-section base |
| API-compat | vms-8208 | §1.4 / §3 — K=0 single-proc image still activates (real N=3 activation assertion) |
| doc cascade | vms-ef12 | §3 + reconcile design-imgact-vms-activation-context.md §3.3 to vms_transfer.S (Rule 10) |
| E2E | vms-d03 | rail rebuilds crtl_rms → N=7 (0x0035A039) on real qemu-system-alpha + /dev/vms |

Any change to the EVAX object/relocation format fires the project's design-change cascade
(API-compat → test-coverage → doc-update); this document is the design record that opens it, and the
`EVAX_R_OVMX_GPDISP` wire form is the labeled OVMX addition the cascade's format-authenticity gate reviews.

---

## Sources (public, clean-room)

- **VSI OpenVMS Calling Standard** — https://docs.vmssoftware.com/vsi-openvms-calling-standard/ —
  §3.1.1 (R27 = procedure value), §3.3 (procedure value = procedure-descriptor address), §3.4.2 (PDSC),
  §3.6.2 (Linkage Section), §3.6.4 (Simple and Bound Procedures).
- **VSI OpenVMS MACRO Compiler Porting and User's Guide** —
  https://docs.vmssoftware.com/vsi-openvms-macro-compiler-porting-and-user-s-guide/ — §2.3.1 (linkage
  section composition: external-var addresses, large constants, linkage pairs, procedure descriptors; the
  linkage pair = {callee PDSC address, entry point address}, resides in the caller's linkage section).
- **VSI OpenVMS Linker Utility Manual** — https://docs.vmssoftware.com/vsi-openvms-linker-utility-manual/
  — image/GST layout (as cited by docs/design-link-native-toolchain.md §4.2).
- OVMX authoritative source (Rule 10 "what it does now"): `gcc/config/alpha/alpha.cc:7749,7767-7777`,
  `src/imgact/arch/alpha/vms_transfer.S:44,61-64`, `src/vmslink/evax_read.c:64-82,273-359`,
  `src/vmslink/evax_read.h:85-91`.
