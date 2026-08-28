# Design — IMGACT VMS Image-Activation Context for the OpenVMS GCC Port crt0

**Item:** vms-f60d · **Lane:** GCC oracle (vms-da0) + main/IMGACT · **Status:** DESIGN (no code)
**Governing frame:** [[vms-ports-build-ladder]] — build OVMX's VMS-compat surface *up* until the
real VMS GCC port (`alpha-dec-vms`) just builds/runs. This rung is the **image-activation calling
convention** the port's crt0 (`libgcc/config/vms/vms-ucrt0.c`, GPL) demands. No Linux argv shim.

**Clean-room (Rule 8):** every VMS-authentic claim below is grounded in *public docs* — the VSI/HPE
OpenVMS Calling Standard, OpenVMS Programming Concepts Manual, and the GPL GCC crt0 source (which is
the authoritative *consumer* of the interface). No VSI/HPE source or binary was read. Where a public
byte-layout does not exist, OVMX defines its own representation and **labels it OVMX-original**.
Fields that need lab-Alpha *observation* to confirm are itemized in §4c — **do not take a lab-Alpha
pod for this**; the Alpha lane holds it for a cluster investigation. These are queued for a later
serialized read.

---

## 1. The port crt0's exact entry contract (from `vms-ucrt0.c`)

Source read from a blobless GCC checkout at `/tmp` (NOT vendored). File header:
`libgcc/config/vms/vms-ucrt0.c`, "VMS crt0 returning Unix style condition codes", GPLv3 + GCC
Runtime Library Exception, contributed by Douglas B. Rupp. Compiled `-mpointer-size=64`
(`#error` guard on `__INITIAL_POINTER_SIZE != 64`).

### 1.1 The transfer address: `__main`

The image's **transfer address** (the routine the VMS image activator calls to start the image) is
`__main`, whose signature is:

```c
int __main (void *progxfer, void *cli_util, void *imghdr, void *image_file_desc,
            unsigned int linkflag, unsigned int cliflag);
```

Six arguments. On IA-64 the symbol is aliased `ELF$TFRADR` (`MAIN_ASM_NAME`); on Alpha it is plain
`__main`. **`__main` itself interprets none of the six args** — it forwards them verbatim to
`decc$main`. So IMGACT's contract is: *present those six values, by the Alpha calling standard, at
the transfer address.* The semantics of each live entirely in `decc$main` (C RTL / DECC$SHR).

### 1.2 What `__main` does (the whole body)

```c
decc$main (progxfer, cli_util, imghdr, image_file_desc,
           linkflag, cliflag, &argc, &argv, &envp);   /* PRODUCES argc/argv/envp */
```

`decc$main` is the DEC C RTL routine that consumes the activation context and **produces** `argc`,
`argv`, `envp`. Note `argc`/`argv`/`envp` are declared `int` here (32-bit) — `decc$main` returns
**32-bit pointers to 32-bit pointer arrays** (the P0/`<4 GB` region), the historical VMS argv shape.

Then `__main` widens if the image is 64-bit:

- `__gcc_main_flags` is a **globalval** (its *address* encodes the flags): bit0 = `MAIN_FLAG_64BIT`,
  bit1 = `MAIN_FLAG_POSIX`. Set at *main()* compile time.
- If `MAIN_FLAG_64BIT`: reallocate `argv64`/`envp64` with `_malloc32(...)` and copy each 32-bit
  pointer widened to 64-bit. Else `argv64 = (char**)(__int64)argv` directly.
- `status = main (argc, argv64, envp64);`

### 1.3 Return-status mapping (`C$_EXIT1`)

If `MAIN_FLAG_POSIX`:
- `status &= 255` (POSIX 0–255 range).
- If nonzero: `status = (__int64)&C$_EXIT1 + ((status - 1) << STS$V_MSG_NO)` — i.e. `C$_EXIT1`
  (a globalval condition value from `errnodef.h`) offset by the exit code shifted into the message
  field (`STS$V_MSG_NO = 3`). Exit code 1 additionally bumps severity to "severe" and OR-s
  `STS$M_INHIB_MSG` (0x10000000) to suppress the run-time error banner.
- If zero: `status = SS$_NORMAL` (1).

Not POSIX: the raw `main()` return is returned as-is. `__main`'s **return value is a VMS condition
value** the activator maps to the process exit — NOT a Unix exit(2) code.

### 1.4 External symbols the crt0 references (the C-RTL surface it assumes)

| Symbol | Origin | OVMX must provide (where) |
|---|---|---|
| `decc$main` | DEC C RTL (DECC$SHR) | **C RTL / GCC lane** — the real producer of argc/argv/envp |
| `_malloc32` | DEC C RTL | C RTL — 32-bit-region allocator |
| `C$_EXIT1` | `errnodef.h` globalval | C RTL / errno message table |
| `__gcc_main_flags` | emitted at `main()` compile | comes from the port's own compile — no OVMX action |

**Key consequence:** IMGACT can present a perfect activation context and the port crt0 still will
not run unless **`decc$main` exists in the OVMX C RTL**. musl (today's DECC$SHR) has no `decc$main`.
That routine is the *counterpart* rung on the C-RTL/GCC-lane side (§4b). IMGACT's scope here is
strictly: build + pass the six-argument VMS activation context by the Alpha calling standard, and
map the returned condition value to process exit.

### 1.5 Which of the six the crt0 "uses"

`__main` uses **none** directly (pure pass-through). Each is consumed by `decc$main`:

| Arg | VMS meaning (public/inferred) | Consumed by `decc$main` to… |
|---|---|---|
| `progxfer` | program transfer address (where control was transferred) | image/self identification |
| `cli_util` | address of the **CLI callback** dispatch vector | call back into DCL to fetch the command line → parse into `argv` |
| `imghdr` | pointer to the image header | read image characteristics/flags |
| `image_file_desc` | VMS string **descriptor** of the image file spec | `argv[0]` / program name when there is no CLI |
| `linkflag` | flag word set by the **linker** | image-characteristic branch |
| `cliflag` | invoked-from-CLI flag | choose CLI-command-line vs. `image_file_desc` argv derivation |

The exact bit meanings of `linkflag`/`cliflag` and the shape of `cli_util`/`imghdr` are **not**
published byte-for-byte; they are §4c lab-Alpha confirmation items (or read from `decc$main`'s own
OVMX implementation, since the GCC lane writes that half and the two must agree).

---

## 2. OVMX's current IMGACT activation path (file:line) — where it transfers control today

Activator: `src/imgact/imgact.c` (aarch64 + x86_64). It is a static-PIE, `-nostdlib`, freestanding
binary registered as the ELF **PT_INTERP** of every OVMX image (`imgact.c:1-32`). Entry stubs:
`src/imgact/arch/{aarch64,x86_64}/start.S`.

Flow:

1. Kernel `execve` maps the main image, loads PT_INTERP (IMGACT), and enters `_start` with the
   **Linux/SysV initial stack** — `{argc, argv…, NULL, envp…, NULL, auxv…, AT_NULL}`, GP registers
   zeroed (`arch/*/start.S` header comment).
2. `_start` passes `sp` to `imgact_bootstrap(sp)` (`start.S`), which parses argc/argv/envp/auxv
   (`imgact.c:1459-1482`), self-relocates (`imgact.c:1485`), and records `g_envp`/`g_argv0`
   (`imgact.c:1486-1488`).
3. Image class is decided by **presence of PT_DYNAMIC** (`imgact.c:1499-1509`):
   - **PT_DYNAMIC present** → legacy ELF DT_NEEDED path (`imgact.c:1511-1533`).
   - **absent** → `activate_symbol_vector(...)` (`imgact.c:1507`) — the OVMX LINK.EXE symbol-vector
     path: bind `.vms$imp` imports by vector index + GSMATCH (`imgact.c:1380-1457`), drive the musl
     C-RTL bootstrap (`drive_crtl_init`, `imgact.c:1369-1375`), set up TLS.
4. **Both paths end identically:** `return at_entry;` (`imgact.c:1508`, `imgact.c:1536`) — the
   kernel-supplied `AT_ENTRY`.
5. `_start` receives that entry address, **zeroes the GP registers, and `br`/`jmp`s to it**
   (`arch/aarch64/start.S`, `arch/x86_64/start.S`) with the pristine Linux stack still in place.
   On x86_64 it also sets `rdx = 0` (no rtld_fini).

**The current transfer convention, precisely:** IMGACT never *calls* the entry — it **tail-jumps**
to it with a Linux SysV initial stack (argc/argv/envp/auxv on the stack) and cleared registers. The
target's own `_start`/crt0 reads argc/argv off the stack, and process exit happens when that crt0
calls `exit_group`. There is **no return to IMGACT** and **no register argument list**.

This is the exact opposite of what the VMS port crt0 needs (§1): a *standard call* with six register
arguments and a *returned condition value*.

---

## 3. THE DESIGN — presenting a genuine VMS activation context to the port transfer address

### 3.0 Scope / target arch

The port is `alpha-dec-vms`, run on **OVMX-Alpha** (Linux-Alpha substrate) [[alpha-oracle-64bit]]
[[alpha-endtoend-linux-alpha]]. The Alpha **calling standard** (PDSC / register conventions) governs
the transfer. IMGACT today has no Alpha backend — this design adds `src/imgact/arch/alpha/` (the
assembly trampoline + arch header) alongside the existing aarch64/x86_64 backends. The
architecture-independent activator body (mapping, `.vms$imp` binding, TLS, C-RTL drive) is reused
unchanged; only the **final transfer** diverges by image class.

### 3.1 A new image class: the VMS-standard image

Today IMGACT sees two classes (PT_DYNAMIC present / absent). We add a third, orthogonal
distinction **within the symbol-vector class**: how the transfer address is entered.

| Class | Marker | Transfer convention |
|---|---|---|
| Legacy ELF | PT_DYNAMIC present | tail-jump to `AT_ENTRY`, Linux stack (unchanged) |
| **OVMX-SysV** (current LINK.EXE images) | `.vms$sv`/`.vms$imp`, no activation flavor | tail-jump to `AT_ENTRY`, Linux stack (unchanged) |
| **VMS-standard** (port images) | `.vms$sv`/`.vms$imp` **+ activation-flavor = VMS_STD** | **standard call** to the transfer address with the six-arg register context; capture returned status |

**How IMGACT distinguishes a port image (the faithful marker).** The VMS-authentic mechanism is the
image header's **transfer address array** — the ordered list of routines the activator invokes at
startup (LIB$INITIALIZE handlers, then the main transfer address). LINK.EXE already owns image
emission. The design adds a new OVMX section, **`.vms$xfer`** (an *OVMX-original carrier*, labeled as
such per Rule 8 — public docs give the transfer-array *semantics* but no x86/Alpha byte layout, same
posture as `.vms$sv`/`.vms$rel`/`.vms$tls` in `ovmx_image.h`):

```c
/* OVMX-original (Rule 8): carrier for the VMS transfer-address array +
 * activation flavor. Semantics public (Calling Std / Prog Concepts); layout ours. */
#define OVMX_XFER_SECTION ".vms$xfer"
#define OVMX_XFER_MAGIC   0x31465358u  /* "XSF1" */
enum ovmx_act_flavor {
    OVMX_ACT_SYSV    = 0,  /* current OVMX crt0: Linux stack, tail-jump  */
    OVMX_ACT_VMS_STD = 1,  /* VMS port crt0: 6-arg standard call, status */
};
struct ovmx_xfer_header {
    uint32_t magic;
    uint32_t flavor;        /* enum ovmx_act_flavor                       */
    uint32_t count;         /* number of transfer entries                 */
    uint32_t reserved;
    /* uint64_t entry_off[count];  image-relative transfer addresses,
       last = main transfer address (__main); earlier = LIB$INITIALIZE */
};
```

LINK.EXE sets `flavor = VMS_STD` when it links a VMS-port image (transfer address = a `decc$main`
crt0). IMGACT reads `.vms$xfer` in `imgact_bootstrap` right where it reads the other `.vms$*`
sections; absence ⇒ `OVMX_ACT_SYSV` ⇒ **exactly the current path, no regression** for every image
we ship today.

### 3.2 Constructing the six-argument context

After `activate_symbol_vector` has bound imports, driven the C-RTL, and set up TLS (the port image
imports `decc$main`/`_malloc32`/`C$_EXIT1` from DECC$SHR — same `.vms$imp` machinery), IMGACT builds
the context instead of returning `at_entry`:

| Arg (reg R16–R21) | OVMX population | Authenticity |
|---|---|---|
| **`progxfer`** | the resolved main transfer address (`base + entry_off[last]` from `.vms$xfer`, i.e. `__main`'s PDSC/PV) | VMS-authentic (it *is* the transfer address) |
| **`cli_util`** | pointer to the **OVMX CLI callback vector** — a small table of entry points into OVMX DCL (`src/vmsdcl`) that `decc$main` calls to retrieve the command line, matching the VMS CLI-callback contract. If `cliflag`=0 (no CLI), pass 0. | Faithful *target*: real callback into OVMX DCL. Vector **struct layout is OVMX-original** (§4b: GCC lane + I agree the shape with `decc$main`). |
| **`imghdr`** | pointer to the mapped image's header info. OVMX images are ELF; pass a pointer to an **OVMX image-descriptor** IMGACT already holds (base, `.vms$sv`, file spec). | **OVMX-original** representation, labeled. `decc$main` reads only what OVMX's `decc$main` reads (§4b). |
| **`image_file_desc`** | a real **VMS string descriptor** (`dsc$descriptor_s`: `{ uint16 length; uint8 dtype=DSC$K_DTYPE_T; uint8 class=DSC$K_CLASS_S; char *pointer; }`) pointing at the image file spec, derived from `AT_EXECFN` / the resolved image path. | **VMS-authentic** — descriptor format is fully public. |
| **`linkflag`** | the value LINK.EXE records for this image (0 until a concrete meaning is grounded). | **OVMX-original** value, labeled; grounded later (§4c). |
| **`cliflag`** | set nonzero when the process was launched by OVMX DCL as a CLI (the normal `RUN`/foreign-command path), 0 otherwise. IMGACT learns this from the launch channel (a well-known env var / a field the DCL image-run path sets). | Faithful *semantics*; the *signal* IMGACT reads is OVMX-original (§4a). |

### 3.3 The Alpha standard call (the transfer, grounded)

Grounded in the OpenVMS Calling Standard (VSI/HPE) — see Sources:

- Integer args in **R16–R21** (`a0`–`a5`); float in F16–F21 (n/a here — all six are integer/pointer).
- **R25 = AI (Argument Information) register**: byte `<7:0>` = `AI$B_ARG_COUNT` = **6**; field
  `<25:8>` = six 3-bit groups, each `AI$K_AR_I64 = 0` (64-bit/sign-extended integer); `<63:26>` = 0.
  So `R25 = 6` (all-integer, six args) — but IMGACT must set it *explicitly* by the documented
  layout, not hard-code `6`.
- **R27 = PV (procedure value)** = address of `__main`'s **procedure descriptor (PDSC)**; the entry
  code address is at **offset 8** in the PDSC. For a symbol-vector image the transfer entry names the
  PDSC (procedure kind), so `.vms$xfer entry_off[last]` resolves to the PDSC; the trampoline loads
  R27 and jumps to `*(PDSC+8)`.
- **R26 = RA (return address)** = a return label inside IMGACT's Alpha trampoline, so control comes
  *back* after `main()`/`__main` returns.
- On return, **R0 = the VMS condition value** `__main` produced (§1.3).

`src/imgact/arch/alpha/start.S` gains a trampoline `imgact_vms_transfer(pdsc, a0..a5)` that loads
R16–R21, sets R25 per the AI layout, sets R27=PV, R26=return label, jumps to `*(PV+8)`, and returns
R0. `imgact_bootstrap` returns a *disposition* to `_start`: for VMS_STD images it calls the
trampoline and then maps R0 → exit; for SysV/legacy it tail-jumps as today.

### 3.4 Mapping the returned condition value to process exit

`__main` returns a **VMS condition value** (`SS$_NORMAL`, or the `C$_EXIT1`-encoded POSIX status, or
raw `main()` return). Faithful behavior = `SYS$EXIT(status)`: on OVMX-Alpha the executive's `$EXIT`
path (via `/dev/vms`) records the condition value as the process completion status and the low bits
map to the Linux exit code. IMGACT must route through the **executive $EXIT**, not a bare
`exit_group(status & 0xff)` — a raw truncation would drop the VMS condition value that DCL's `$STATUS`
is supposed to observe (INV-6 honesty: don't fake the VMS-observable exit). Where the executive
`$EXIT` is not yet reachable from IMGACT, this is a declared gap (§4a), not a silent shim.

### 3.5 Interaction with the static-PIE / PT_INTERP path

- The port image **stays a PT_INTERP image** activated by IMGACT — no separate loader. It is still
  a LINK.EXE symbol-vector `.EXE` (no PT_DYNAMIC), importing `decc$main`/`_malloc32`/`C$_EXIT1` from
  DECC$SHR via `.vms$imp`. All existing mapping/binding/TLS/C-RTL-drive code is reused verbatim.
- The **only** divergence is the final transfer: flavor `VMS_STD` ⇒ standard call + status capture;
  flavor `SYSV`/legacy ⇒ today's tail-jump. The `.vms$xfer` marker is what selects it; its absence
  is the current behavior, so **no shipped image changes**.
- The port image's crt0 does **not** read the Linux initial stack — but IMGACT (the PT_INTERP) has
  already consumed argc/argv/envp/auxv from that stack for its own bootstrap and the C-RTL init, so
  the Linux stack reality is fully absorbed *inside the activator*. The port crt0 sees only the VMS
  six-arg register context. This is the faithful boundary: Linux reality stops at IMGACT; the image
  sees VMS. (Same principle as the tmpfs boot-bridge / [[files11-acp-pivot]].)
- **C-RTL TLS ownership** is unchanged: `find_crtl_producer`/`drive_crtl_init` still run (musl owns
  TP); the port image's own TLS is absorbed by `setup_producer_tls_over_crtl`/`g_exe`
  (`imgact.c:1441-1449`, `absorb_tls_over_crtl`). The standard call happens *after* that, so
  `decc$main`/`main` run with a fully-initialized C-RTL.

---

## 4. Open questions

### 4a. Need MY (conductor) decision

1. **`.vms$xfer` transfer-address array vs. a header flag.** I propose the full transfer-address
   array (faithful to VMS's LIB$INITIALIZE-then-main model) rather than a bare one-bit flag, so the
   same mechanism later carries LIB$INITIALIZE handlers. Confirm we build the array now (even if
   `count==1` initially) rather than retrofitting later. *Recommendation: build the array.*
2. **`$EXIT` routing from IMGACT (§3.4).** Should IMGACT reach the executive `$EXIT` over `/dev/vms`
   to record the VMS condition value, or is a declared-gap interim (exit_group with low-bit map +
   a filed follow-up item) acceptable for first-light? *Recommendation: declared-gap interim behind
   a filed item; full `$EXIT` before the port is called "done".* This touches INV-6 — flag for
   ruling.
3. **`cliflag`/`cli_util` signal source (§3.2).** How does IMGACT learn "launched as a CLI"? Options:
   (a) an env var the DCL image-run path sets; (b) a field in the executive process context read over
   `/dev/vms`. *Recommendation: (b) — the executive already owns the process/CLI relationship; an env
   var is a Linux shim.* Confirm.
4. **Does this rung gate a release, or is it post-1.0 (GCC lane is post-1.0)?** Per
   [[gcc-oracle-lane]] the lane doesn't gate 0.5. Confirm this activation rung inherits that.

### 4b. Need GCC-lane input (the port side — the other half of the contract)

5. **`decc$main` must exist in the OVMX C RTL.** musl has none. The GCC lane owns implementing a
   faithful `decc$main(progxfer, cli_util, imghdr, image_file_desc, linkflag, cliflag, &argc, &argv,
   &envp)` that derives argc/argv/envp — via the `cli_util` callback when `cliflag` set, else from
   `image_file_desc`. This is the load-bearing counterpart; IMGACT's context is inert without it.
6. **`_malloc32` + the 32-bit-pointer argv contract.** `decc$main` returns 32-bit-pointer argv/envp
   arrays; `_malloc32` allocates in the `<4 GB` region. On Linux-Alpha (LP64) this requires a low
   (`MAP_32BIT`-style) allocation region. Agree who owns it (C RTL) and that IMGACT need not.
7. **`cli_util` vector shape + `imghdr` contents.** These two are OVMX-original structs; the GCC
   lane's `decc$main` and my IMGACT must define the *same* layout. Co-design the two structs.
8. **`linkflag` meaning.** Confirm whether the port's `decc$main` branches on `linkflag` at all; if
   not, IMGACT passes 0 and we defer grounding it.

### 4c. Need lab-Alpha oracle confirmation (LATER — do NOT take a pod now)

Queue for a serialized read when the Alpha lane frees lab-Alpha ([[alpha-oracle-64bit]]):

9. **The exact six-arg activation context a real OpenVMS Alpha image receives** at its transfer
   address (observe via a tiny MACRO/BLISS or C image that dumps R16–R21/R25/R27 at entry) — to
   confirm our field population matches what genuine VMS presents, especially `cliflag`/`linkflag`
   bit meanings and whether `imghdr` points at the real image header vs. an activation block.
10. **The `C$_EXIT1`/`$STATUS` round-trip** — run an image that `exit()`s with a known code under
    OpenVMS Alpha and read DCL `$STATUS`/`$SEVERITY`, to confirm §3.4's condition-value mapping.
11. **PDSC kind + `.vms$xfer` fidelity** — confirm the transfer entry resolves to a real
    procedure descriptor (offset-8 entry) on Alpha, so R27/PV setup matches.

Everything else in this design is grounded in public docs + the GPL crt0; the §4c items are
*confirmation*, not blockers to starting implementation against the documented standard.

---

## 5. Tie to R8 (the Alpha PDSC/CHF/invocation-context calling-standard rung)

The [[vms-ports-build-ladder]] frames faithfulness as a ladder of VMS-compat rungs. **R8 is the
Alpha calling-standard rung**: PDSC (procedure descriptors), the CHF (Condition Handling Facility)
invocation context, and the standard-call register conventions (R16–R21 / R25-AI / R26-RA / R27-PV).
This design **is the activation-time face of R8**:

- The transfer in §3.3 is a **standard call** built on exactly R8's PDSC + register conventions —
  IMGACT is the first OVMX component that must *issue* an Alpha standard call to arbitrary VMS-port
  code, so it forces R8's PDSC/PV/AI handling into existence.
- The returned **VMS condition value** (§1.3/§3.4) is the CHF's currency; routing it through `$EXIT`
  and making it visible to DCL `$STATUS` is the activation-time slice of R8's condition model.
- The **`.vms$xfer` transfer-address array** (§3.1) is the image-header side of R8: the ordered
  invocation list (LIB$INITIALIZE handlers → main transfer) that the calling standard + Programming
  Concepts describe.

Downstream of R8, full CHF (`$ESTABLISH`/unwind/signal arrays, PDSC handler-descriptor fields)
remains its own rung; this design only requires the **entry-side** PDSC/PV/AI/RA subset plus
condition-value-as-exit. Full CHF is a declared follow-on, not in scope here.

---

## Sources (public docs — Rule 8)

- VSI OpenVMS Calling Standard — https://docs.vmssoftware.com/vsi-openvms-calling-standard/ (R16–R21
  argument registers, R25 AI, R26 RA, R27 PV).
- OpenVMS Alpha Calling Standard / Programming Concepts (AI register `AI$B_ARG_COUNT` `<7:0>`,
  `AI$V_ARG_REG_INFO` `<25:8>` six 3-bit groups, `AI$K_AR_I64=0`; PDSC entry at offset 8) —
  https://www.digiater.nl/openvms/doc/alpha-v8.3/83final/5841/5841pro_053.html
- OpenVMS Programming Concepts Manual — image activator reads the transfer address and passes control
  to it — https://www.digiater.nl/openvms/doc/alpha-v8.3/83final/5841/5841pro_050.html
- GCC (GPLv3 + Runtime Library Exception) `libgcc/config/vms/vms-ucrt0.c` — the port crt0 contract
  (`__main`/`decc$main`/`C$_EXIT1`). Read from a blobless checkout at `/tmp`; **not vendored**.
