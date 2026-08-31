# Design: Real VMS Condition-Handling Dispatch (CHF)

- **rd item:** `vms-2e72` (gap register R8 — see `docs/design-gcc-port-surface-gaps-register.md`)
- **Status:** rung-1 landed; rungs 2–5 filed as children of `vms-2e72`.
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
| G4 | `SYS$UNWIND` runs intervening handlers in `CHF$V_UNWINDING` mode and **transfers control** to a target frame at `newpc` | pops the side array; `newpc` ignored; intervening frames not abandoned |
| G5 | Alpha ICB primitives (`LIB$GET_INVO_*`) expose the chain | absent |
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

### Rung-2 — real machine-frame-transfer `SYS$UNWIND` *(child: G4)*

Make `sys$unwind(depadr, newpc)` abandon intervening machine frames and resume in
the target frame, running each intervening non-active handler once with
`CHF$V_UNWINDING` set. Because OVMX invokes handlers as nested calls from within
`lib$signal`, a faithful transfer needs a resumable context anchored at the
**establisher** frame. Approach: `lib$establish` captures a resumable context for
the establisher frame (a `sigjmp_buf`-class save via a thin establish shim that
runs in the establisher's frame, preserving the `lib$establish(handler)` source
signature); `sys$unwind` runs the unwinding handlers, pops the chain, then
transfers to the target frame's saved context (`newpc` honoured when non-zero).
**Test:** port `tests/corpus/tier1-examples/sys_unwind.c` to an executable
assertion — the `SS$_ABORT` case must NOT print "After abort" (control returns to
`main`), which the current pop-only emulation cannot achieve.

### Rung-3 — Alpha invocation-context primitives *(child: G5)*

`LIB$GET_INVO_CONTEXT` / `LIB$GET_PREV_INVO_CONTEXT` / `LIB$GET_INVO_HANDLE` over
an invocation-context block, walking the **genuine** Alpha frame chain (procedure
descriptors / register save areas per the Calling Standard), so the handler search
and unwind consult the real chain rather than the `lib$establish` side-chain.
Width-sensitive → 3-way / Alpha-LP64 gated. **Test:** an Alpha-rig program that
walks its own invocation chain and matches frame count/handles against the oracle.

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
