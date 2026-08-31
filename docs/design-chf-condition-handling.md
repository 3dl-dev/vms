# Design: Real VMS Condition-Handling Dispatch (CHF)

- **rd item:** `vms-2e72` (gap register R8 — see `docs/design-gcc-port-surface-gaps-register.md`)
- **Status:** rung-1, rung-2 and rung-3 landed (rung-3 = the host-proven Alpha
  invocation-context walk engine + primitives + the anchorless-unwind wiring;
  the real Alpha register-file capture and the machine register-restore transfer
  are the deferred Alpha-runtime children); rungs 4–5 filed as children of
  `vms-2e72`.
- **Rule:** VMS-compatibility-first (Rule 1, "do it like VMS"); Rule 9 / INV-6
  (real executive path, no per-process fake); clean-room (Rule 8 — this design is
  from the public OpenVMS Programming Concepts Manual + Calling Standard + System
  Services Reference, never from VSI/HPE source).

> **Tracking note (SSOT conflict, flagged):** the gap register row **R8**
> assigns `vms-2e72` to *"Real VMS condition-handling dispatch"*, and this is the
> work item the GCC-port ladder depends on (`vms-3e4` → `vms-fd1`). The rd
> database record for `vms-2e72` currently carries a **different** title/body
> ("RMS surface beyond basic stdio…"), created 2026-08-31. The register is the
> design SSOT for the CHF assignment; this doc + the implementation are the CHF
> work. The RMS-surface text needs its own item id. Raised in the PR / report so
> the operator can reconcile the rd record — not silently overwritten.

---

## 1. What real VMS does (the target)

On OpenVMS a **condition** is signalled either by software
(`LIB$SIGNAL` / `LIB$STOP`, or a system service returning through the signal
path) or by a **hardware exception** the CPU delivers to the executive, which
maps it to an `SS$_` condition code (e.g. an Alpha arithmetic trap → `SS$_HPARITH`,
a memory fault → `SS$_ACCVIO`). Either way the flow is identical from the
**exception dispatcher's** point of view:

1. **Build the signal array and mechanism array.**
   - *Signal array* — describes *what* happened: the condition value plus any
     signal-specific arguments (FAO args) and, on a real signal, the trailing PC
     and PSL at the point of the exception.
   - *Mechanism array* — describes *where* it happened and carries the state
     needed to resume/return: the establisher **frame** pointer, the call
     **depth**, saved R0/R1, and flags (notably the *unwinding* bit).

2. **Search for a handler, in a fixed order:**
   1. the **primary** software exception vector (`SYS$SETEXV`),
   2. the **call-frame handlers**, walking the invocation chain from the
      innermost (signalling) frame outward toward the image's outermost frame —
      each frame's established handler found via the frame's handler slot
      (VAX) / the invocation-context + registered handler (Alpha),
   3. the **secondary** software exception vector,
   4. the **last-chance** software exception vector,
   5. the catch-all (traceback / image exit).

3. **Honour the handler's return:**
   - `SS$_CONTINUE` — stop the search; return to the point of signal and resume.
   - `SS$_RESIGNAL` — resume the search at the next handler in the order above.
   - **Unwind** — the handler calls `SYS$UNWIND(depadr, newpc)`. The dispatcher
     then walks the invocation chain from the current frame down to the **target**
     frame (named by `depadr`, a depth taken from `chf$is_mch_depth`), calling
     each intervening established handler one last time with the
     **`CHF$V_UNWINDING`** flag set so it can release resources, then **transfers
     control** by restoring the target frame's saved registers and setting the PC
     to `newpc` (or the target's return PC when `newpc` is 0). Intervening frames
     are abandoned; execution resumes in the target frame.

The Alpha calling standard exposes the invocation chain to programs via
`LIB$GET_INVO_CONTEXT` / `LIB$GET_PREV_INVO_CONTEXT` / `LIB$GET_INVO_HANDLE`
(operating on an *invocation context block*, ICB), and `SYS$UNWIND` /
`LIB$SIGNAL` are defined in terms of it.

---

## 2. What OVMX had (the emulation) and the gap

`src/libvms/rtl/lib_signal.c` (pre-`vms-2e72`) kept a **thread-local flat array**
of handler function pointers (`handler_stack[]`). `lib$establish` pushed a pointer;
`lib$signal`/`lib$stop` walked that array top-to-bottom calling each handler; the
mechanism array was **stubbed** (`chf$ph_mch_frame = NULL`, `chf$is_mch_depth =
handler_count`, saved registers = 0). `SYS$UNWIND` (`syssvc/sys_condition.c`) only
**popped** that array to a target count — its `newpc` argument was accepted and
ignored (no machine frame to transfer to). There were **no** software exception
vectors: `SYS$SETEXV` did not exist.

Gap versus the target, itemised:

| # | Authentic behaviour | Emulation (pre-2e72) |
|---|---------------------|----------------------|
| G1 | primary/secondary/last-chance vectors via `SYS$SETEXV`, searched around the frame chain | absent entirely |
| G2 | mechanism array carries the **real** establisher frame + depth + saved regs | `NULL` frame, `handler_count` as depth, zero regs |
| G3 | handler search follows the **real invocation (frame) chain** | walks a side array unrelated to actual frames |
| G4 | `SYS$UNWIND` runs intervening handlers in `CHF$V_UNWINDING` mode and **transfers control** to a target frame at `newpc` | **closed (rung-2)** for an anchored target frame: deferred unwind, intervening `CHF$V_UNWINDING` calls, `setjmp`/`longjmp` transfer, `newpc` honoured, frames abandoned. Resuming into an *un-anchored* ancestor frame → rung-3 (G5). |
| G5 | Alpha ICB primitives (`LIB$GET_INVO_*`) expose the chain | **closed (rung-3)** on the host: real PDSC/RSA walk (`rtl/lib_invo.c`) reconstructs each caller's PC/FP/SP/handler/bottom from constructed Alpha frames; `perform_unwind` consults it on the anchorless path. Real Alpha register capture + the machine transfer are the deferred children. |
| G6 | one dispatcher serves both software signals **and** hardware exceptions | software path only; the `SS$_HPARITH` bridge (`vms-db3`/GAP3) proves the HW→condition→handler-chain pattern for one code but is not unified |

The narrower `SS$_HPARITH` FP-trap bridge (`src/libvms/rtl/arith_signal.c`,
`vms-db3`, done) is **prior art**: it already delivers a real hardware trap
(SIGFPE, decoded from `sc_fpcr` — see the gap3 memory note) as a faithful
`lib$signal(SS$_HPARITH, 5, …)` that flows through this same handler chain to
`$STATUS`. It proves the pattern for one condition; the full CHF machinery below
is what the GCC port's own `libgcc/config/alpha/vms-unwind.h` needs for real
exception unwinding.

---

## 3. Array layouts (as implemented / targeted)

Structures live in `src/libvms/include/chfdef.h`.

**Signal array** (`struct chf$signal_array`):

```
[0] chf$is_sig_args   count of longwords that follow
[1] chf$is_sig_name   condition value (SS$_ code)
[2] chf$is_sig_arg1   first signal-specific / FAO argument
...                   further arguments
[N-1] PC              (real-exception path) PC at point of signal   [rung-4]
[N]   PSL             (real-exception path) processor status        [rung-4]
```

**Mechanism array** (`struct chf$mech_array`):

```
chf$is_mch_args    argument count (5 = standard)
chf$is_mch_flags   flags; bit CHF$V_UNWINDING set during an unwind
chf$ph_mch_frame   establisher FRAME pointer         -- REAL as of rung-1
chf$is_mch_depth   establisher call depth            -- REAL as of rung-1
chf$is_mch_savr0   saved R0 (handler may set the return value)
chf$is_mch_savr1   saved R1
```

**Exception-vector selectors** (`SYS$SETEXV` first argument):
`CHF$K_PRIMARY_VECTOR` (0), `CHF$K_SECONDARY_VECTOR` (1),
`CHF$K_LAST_CHANCE_VECTOR` (2).

**Mechanism flags:** `CHF$V_UNWINDING` (bit 0), `CHF$M_UNWINDING`.

---

## 4. Phased implementation plan

### Rung-1 — authentic dispatch structure + exception vectors *(this PR)*

Replaces the emulation's dispatch with the real CHF search order and real
mechanism-array construction, and adds the missing exception-vector service.

- **`SYS$SETEXV`** (`src/libvms/syssvc/sys_setexv.c`, declared in `starlet.h`) —
  real per-thread primary/secondary/last-chance vector table; returns the
  previously established handler; `SS$_BADPARAM` for a bad selector. Single-mode
  userspace model documented honestly (the `acmode` argument is validated but one
  effective slot participates — this is a stated simplification, not a per-process
  fake of a shared facility; INV-6).
- **Frame-anchored handler records** (`lib_signal.c`) — `lib$establish` captures
  the establisher's **real** frame address and return PC
  (`__builtin_frame_address(1)`/`__builtin_return_address(0)`; `lib$establish` is
  a genuine exported, non-inlined function so frame(1) names the caller). The
  mechanism array now reports **G2** truthfully.
- **Authentic search order** (`dispatch_condition()`): primary vector → frame
  chain innermost→outermost → secondary vector → last-chance vector → default.
  Handler re-entrancy guard (an `active` flag) so a handler that re-signals is not
  re-entered. Shared by `lib$signal` and `lib$stop`.
- **Test:** `tests/libvms/test_condition_dispatch.c` (host-buildable, arch-generic)
  proves the `SYS$SETEXV` API, the four-stage search order, the real
  mechanism-array frame/depth, and `CONTINUE`/`RESIGNAL` at each stage.
- **Regression:** `tests/libvms/test_lib_fb3.c` (`sys$unwind` chain-pop
  contract) unchanged — 29/29.

Closes G1, G2, and G3 for the software-signal path.

### Rung-2 — real machine-frame-transfer `SYS$UNWIND` *(landed; G4)*

`sys$unwind(depadr, newpc)` now performs a real machine-frame transfer: it
abandons the intervening machine frames and resumes in the target frame, running
each intervening non-active handler once with `CHF$V_UNWINDING` set, honouring
`newpc`.

- **Deferred-unwind model.** As on real VMS, `sys$unwind` does **not** transfer
  immediately — it records the request (target depth + `newpc`) and returns to the
  handler; the dispatcher (`dispatch_condition`) performs the transfer when the
  handler returns (`perform_unwind`, `src/libvms/rtl/lib_signal.c`). This is what
  keeps a handler that ends `sys$unwind(...); return SS$_CONTINUE;` (as the corpus
  and `test_lib_fb3` both do) well-defined.
- **Resume anchor.** Because OVMX invokes handlers as nested calls from within
  `lib$signal`, the transfer target is a resumable context **anchored in the
  target frame**. A procedure that wants `SYS$UNWIND` to transfer back into its
  frame arms one with `VMS$UNWIND_ANCHOR()` (a `setjmp` macro that runs in the
  establisher's own frame — `chfdef.h`) right after `lib$establish`. The
  dispatcher `longjmp`s to it, abandoning the intervening frames. `newpc` is
  carried through and readable at the resume site via `vms$$unwind_newpc()`.
- **Compatibility.** A `NULL` `depadr`, a target frame that armed no anchor, or a
  `sys$unwind` call made outside an active dispatch keeps the historical pop-only
  handler-chain contract — so `test_lib_fb3` (29/29) is unchanged.
- **Test:** `tests/libvms/test_condition_unwind.c` — the executable-assertion port
  of `tests/corpus/tier1-examples/sys_unwind.c`. The `SS$_ABORT` case asserts the
  code after the signal does **not** run (the corpus's "After abort" that never
  prints), that intervening frames are abandoned, that an intervening handler is
  called once with `CHF$V_UNWINDING`, and that `newpc` is honoured.

> **Honest host scope (→ rung-3).** The target frame must have armed a resume
> anchor. Resuming into an ancestor frame that armed **no** anchor — e.g. the
> corpus's literal "return to `main`", a bare caller of the establisher that has
> no OVMX hook — needs the real machine invocation-context walk (procedure
> descriptors / register save areas) so the runtime can reconstruct and restore
> that frame's context. That is exactly rung-3 (`vms-1fa`, `LIB$GET_INVO_*`);
> until it lands, a target frame declares its resumability with
> `VMS$UNWIND_ANCHOR()`. No silent scope drop: the frame-transfer *mechanism*
> (deferred unwind, intervening `CHF$V_UNWINDING` calls, `longjmp` transfer,
> `newpc`, chain pop) is real and complete on the host; only the source of the
> target frame's saved context (explicit anchor now vs. walked invocation chain in
> rung-3) differs.

### Rung-3 — Alpha invocation-context primitives *(landed; G5)*

`LIB$GET_CURR_INVO_CONTEXT` / `LIB$GET_PREV_INVO_CONTEXT` /
`LIB$GET_INVO_CONTEXT` / `LIB$GET_INVO_HANDLE` / `LIB$GET_PREV_INVO_HANDLE`
(`src/libvms/rtl/lib_invo.c`, structures in `src/libvms/include/pdscdef.h`) walk
the **genuine** Alpha call chain — procedure descriptors (PDSC) + register save
areas (RSA) per the Alpha Calling Standard — so the runtime can reconstruct any
frame's saved context (PC, FP=R29, SP=R30, preserved registers, established
handler) rather than only the `lib$establish` side-chain.

- **Walk engine (host-proven).** `vms$$invo_walk_prev` dispatches on the PDSC
  kind: a **register-frame** (leaf) procedure's caller PC comes from the
  return-address register (R26); a **stack-frame** procedure's caller is
  reconstructed from the RSA at `frame_base + pdsc$w_rsa_offset` — the saved
  return address plus every integer register named in `pdsc$l_ireg_mask`,
  restored in ascending register order (recovering the caller's FP). Bottom of
  stack is a 0 saved-return or an unresolved PC. A pluggable PDSC resolver is
  the image-linkage seam (real image lookup on Alpha; a constructed table under
  the host test). This is pure 64-bit control flow — proven host-side against
  faithfully constructed Alpha frames (`tests/libvms/test_invo_context.c`,
  28/28: stack walk, FP/handler/bottom reconstruction, handle round-trip,
  register-frame hop, anchorless reconstruction).
- **Anchorless-unwind wiring.** `perform_unwind` (rung-2, `lib_signal.c`), on a
  target frame that armed **no** `VMS$UNWIND_ANCHOR`, now calls
  `vms$$invo_reconstruct_target` to walk to that frame and rebuild its context,
  then `vms$$invo_transfer` to resume there. On the host `vms$$invo_transfer`
  honestly reports it cannot machine-restore an Alpha frame, so the rung-1
  pop-only contract stands (`test_lib_fb3` 29/29, `test_condition_unwind` 13/13
  unchanged).
- **Deferred to the Alpha-runtime children (`vms-cc8` bracket / `vms-8e8c`):**
  `LIB$GET_CURR_INVO_CONTEXT` capturing a **real** Alpha register file, the
  genuine image PDSC-lookup resolver, and the machine register-restore + jump in
  `vms$$invo_transfer` — proven on qemu-alpha with an Alpha-rig program that
  walks its own chain and matches frame count/handles against the oracle, and
  the libgcc-EH landing-pad resume that this enables.

### Rung-4 — unify hardware-exception dispatch *(child: G6)*

Route the hardware-exception path (extend `arith_signal.c`'s model to `SS$_ACCVIO`
and friends) through the **same** `dispatch_condition()`, building the signal
array's trailing PC/PSL and the mechanism array's saved registers from the real
`mcontext`. Proven on real `/dev/vms` / qemu-alpha (not qemu-user) per the P1
harness rule. **Test:** Alpha-rig `test_arith_*`-class suites (off the negctl
glob per the gap3 note) that fault, dispatch through a `SYS$SETEXV` primary
vector + a frame handler, and assert the search order on a real trap.

### Rung-5 — libgcc EH integration *(child)*

Make the OpenVMS GCC port's `libgcc/config/alpha/vms-unwind.h` resolve against
this CHF machinery so C++ EH / `__builtin_unwind` and language error paths work
against OVMX's DECC$SHR with no port-source patch (`vms-3e4` outcome).

---

## 5. Test strategy summary

| Rung | Layer | Where | Gate |
|------|-------|-------|------|
| 1 | dispatch order + vectors + mech array | `tests/libvms/test_condition_dispatch.c` | host ctest (arch-generic) |
| 2 | frame-transfer unwind | corpus `sys_unwind.c` → executable | host ctest |
| 3 | invocation-context walk | Alpha rig | Alpha LP64 oracle |
| 4 | HW-exception unification | `tests/qemu` `test_arith_*` | real `/dev/vms` / qemu-alpha |
| 5 | libgcc EH | joint-e2e alpha | Alpha cross toolchain |

Rungs 1–2 are host-provable and arch-generic (dispatch order and frame transfer
are pure control flow). Rungs 3–5 are inherently Alpha/width-sensitive and gate on
the Alpha oracle + real executive per Rule 5 and the P1-harness rule.
