# OVMX Roadmap: OpenVMS Alpha Source Compatibility

## Context

OVMX has Phases 1-7 complete — a working VMS-inspired environment with 56 system services, 49 RTL functions, full descriptors, RMS, DCL, kernel modules, and a bootable distro. But it has never been tested against real VMS source code. The project goal is now clear: **OpenVMS Alpha source compatibility** — real VMS C programs compile against OVMX headers and run correctly. Follow-on targets: Fortran, BASIC, BLISS, and DECnet Phase IV.

The roadmap is driven by a test corpus, not a feature list. Each phase is validated by compiling real VMS programs. If it compiles and runs, we're compatible. If it doesn't, the error messages tell us what to build next.

---

## Phase Dependency Graph

```
Phase 8: Conformance Audit + Core Gap Closure
    |
Phase 9: CRTL + Header Fidelity
    |
    +------------+------------+
    |            |            |
Phase 10     Phase 11     Phase 12
(MMK)        (Fortran)    (BASIC)
    |
Phase 13: DECnet Phase IV + BLISS
```

Phases 10/11/12 can run in parallel after Phase 9. DECnet depends on Phase 10 (proven sys$qio + process management). BLISS is independent.

---

## Phase 8: Conformance Audit + Core Gap Closure

**Goal**: Compile 50+ Eight-Cubed example programs. Every failure reveals an API gap.

**Scope**: Large (6-8 weeks) | **Depends on**: Nothing

### Deliverables

1. **Conformance test harness** (`tests/conformance/`)
   - Fetch/include Eight-Cubed C examples (~200 programs, 50-100 LOC each)
   - Automated build-and-run: compile each against OVMX headers, capture errors, run, check status
   - Machine-readable pass/fail reporting
   - Source: https://www.eight-cubed.com/examples.shtml

2. **sys$fao / sys$faol** — Formatted ASCII Output as system service
   - FAO engine already exists in `lib$sys_fao` (src/libvms/rtl/lib_datetime.c)
   - Expose as `sys$fao` in starlet.h / sys_misc.c (thin wrapper)
   - Used by nearly every VMS program that formats output

3. **sys$getmsg / sys$putmsg** — Message retrieval and display
   - Needs a message database mapping condition codes to text
   - Required by all VMS programs that do error reporting

4. **sys$synch** — Wait for async system service completion
   - Wait on event flag + check IOSB
   - Trivial but ubiquitous — blocks compilation of many programs

5. **Condition Handling Facility (CHF)** — Full implementation
   - Current state: `lib$establish` declared but `lib$signal` just prints and exits
   - Needed: thread-local handler stack, signal dispatch, handler return codes
   - New header: `chfdef.h` (chf$signal_array, chf$mech_array)
   - `lib$establish` pushes handler, `lib$signal` walks stack calling handlers
   - Handlers return SS$_CONTINUE (dismiss) or SS$_RESIGNAL (pass to next)
   - `sys$unwind` / `sys$goto_unwind` via setjmp/longjmp
   - Files: rewrite `src/libvms/rtl/lib_signal.c`, new `chfdef.h`

6. **Missing system services**: sys$forcex, sys$suspend, sys$resume, sys$setpri, sys$cancel

7. **Missing header constants** — Audit for gaps:
   - `jpidef.h` (JPI item codes — many exist in prcdef.h, may need dedicated header)
   - `syidef.h` (SYI item codes)
   - `prvdef.h` (privilege bit definitions)
   - `chfdef.h` (condition handler frame)
   - `msgdef.h` (message flags)
   - `libclidef.h` (CLI symbol types)

8. **Missing RTL functions**:
   - `lib$find_file` / `lib$find_file_end` (wildcard file search)
   - `lib$get_symbol` / `lib$set_symbol` (CLI symbols)
   - `str$match_wild` (wildcard pattern matching)
   - `str$analyze_sdesc` (analyze string descriptor)
   - `str$replace` (replace substring)
   - `mth$sinh/cosh/tanh` and other hyperbolic functions

### Validation
- Target: 50+ Eight-Cubed examples compiling and running
- Track pass/fail ratio as the primary metric
- Each failure cataloged as a bead with specific missing API

### Key Files
- `src/libvms/include/starlet.h` — extend with new sys$ declarations
- `src/libvms/rtl/lib_signal.c` — rewrite for full CHF
- `src/libvms/syssvc/sys_misc.c` — sys$fao, sys$getmsg, sys$synch
- `tests/conformance/` — new harness

---

## Phase 9: CRTL (VMS C Runtime Library) + Header Fidelity

**Goal**: VMS C programs using standard stdio with VMS file semantics compile and work.

**Scope**: Large (6-8 weeks) | **Depends on**: Phase 8

### Deliverables

1. **VMS-compatible CRTL shim** — NEW library: `src/vmscrtl/`
   - Wraps POSIX stdio calls to route through VMS logical name translation + RMS
   - `fopen("SYS$LOGIN:DATA.TXT", "r")` translates logicals, parses VMS filespecs
   - `fopen("file.dat", "r", "rfm=var", "rat=cr")` — VMS optional RMS arguments
   - NOT a libc replacement — a thin interception layer above glibc
   - Architecture:
     ```
     VMS C program → vmscrtl (libvmscrtl.so) → vmsfs (logical names) → glibc
     ```

2. **DECC$ feature switches** — `decc$feature_get` / `decc$feature_set`
   - Implement the 10-15 most common (DECC$FILENAME_UNIX_ONLY, DECC$EFS_CHARSET, etc.)
   - Controlled by logical names (matching VMS behavior)

3. **vaxc$errno** — VMS programs check this alongside `errno`
   - CRTL sets both errno (POSIX) and vaxc$errno (VMS status code)

4. **Header binary compatibility audit**
   - Compare OVMX structs against real OpenVMS Alpha headers (available in HP/VSI hobbyist kit)
   - Critical structs: `dsc$descriptor_s`, `FAB`, `RAB`, `NAM`, `item_list_3`
   - Any padding, field ordering, or size differences cause silent data corruption
   - FAB has internal OVMX fields (`_linux_fd`, `_resolved_path`) — must not change public struct size

5. **Missing type headers**: `builtins.h`, `unixlib.h`, VMS-extended `signal.h` / `errno.h` / `stat.h`

### Validation
- Eight-Cubed programs using stdio file I/O compile and run
- Begin attempting compilation of simpler MMK source files

### Key Files
- `src/vmscrtl/` — new library (CMakeLists.txt, vmscrtl_stdio.c, vmscrtl_decc.c)
- `src/vmsrms/include/rms/fab.h` — audit for binary compat, separate internal state
- `src/libvms/include/descrip.h` — audit against real VMS

---

## Phase 10: MMK Build Target

**Goal**: Compile and run MMK (MadGoat Make), a real 57-file VMS application.

**Scope**: Medium (4-6 weeks) | **Depends on**: Phases 8-9

### Deliverables

1. **MMK-specific API gaps** — Audit MMK source for all sys$/lib$/str$ calls, close gaps:
   - `sys$filescan` — lightweight filespec parsing (different from sys$parse)
   - `cli$present` / `cli$get_value` / `cli$dcl_parse` — command line parsing from DCL command definition tables
   - `lib$get_foreign` — get foreign command line (argc/argv equivalent)
   - `lib$find_file` / `lib$find_file_end` — if not completed in Phase 8

2. **CLI$ routines** — Minimal implementation
   - Parse argc/argv into VMS-style qualified command parameters
   - Enough to satisfy MMK's command-line parsing (not full CLD table support)

3. **Subprocess logical name inheritance** — Verify lib$spawn correctly propagates process logical names

### Validation
- MMK compiles with OVMX headers, links against OVMX libraries
- Successfully processes a simple MMS description file to build a target
- Begin attempting GNV tools (grep, sed) — identifies next gaps

### Key Files
- `src/libvms/syssvc/sys_filescan.c` — new
- `src/libvms/rtl/lib_cli.c` — new (cli$ routines, lib$get_foreign)

---

## Phase 11: Fortran Support

**Goal**: VMS Fortran programs compile with GFortran + OVMX extensions.

**Scope**: Large (6-8 weeks) | **Depends on**: Phase 9 (CRTL)

### Strategy
GFortran handles standard Fortran. A **source-to-source preprocessor** translates VMS extensions before GFortran sees the code. A **Fortran binding layer** provides VMS system service access.

```
VMS Fortran (.FOR) → vmsfortran preprocessor → GFortran → links against OVMX + libvmsfortran
```

### Deliverables

1. **Fortran preprocessor** (`tools/vmsfortran/`)
   - `%REF`, `%VAL`, `%LOC`, `%DESCR` → ISO_C_BINDING equivalents
   - `USEROPEN` attribute → RMS user-open callback
   - `RECORDTYPE`, `RECORDSIZE`, `CARRIAGECONTROL` on OPEN → RMS FAB attributes
   - `STRUCTURE` / `RECORD` → Fortran 90 derived types
   - Tab-format source handling

2. **Fortran-to-OVMX RMS bindings** (`src/vmsfortran/`)
   - Fortran module (`vms_rms.f90`) wrapping RMS sys$ calls
   - `for$open`, `for$close`, `for$read_seq`, `for$write_seq` (DEC Fortran internal RTL)
   - OPEN/READ/WRITE with VMS record attributes

3. **Descriptor translation layer**
   - GFortran CHARACTER arguments use GFortran's internal descriptor format
   - Binding layer translates to/from VMS `dsc$descriptor_s` when calling OVMX services

### Validation
- Classic VMS Fortran programs: matrix ops, file I/O with RMS attributes, STRUCTURE/RECORD usage
- Compile with GFortran + preprocessor, run correctly

---

## Phase 12: BASIC Transpiler

**Goal**: DEC BASIC / BASIC-PLUS-2 programs compile via BASIC-to-C transpiler.

**Scope**: Medium (4-6 weeks) | **Depends on**: Phase 9 (CRTL), Phase 10 (RMS proven)

### Strategy
Transpile DEC BASIC to C, linking against OVMX libraries.

```
DEC BASIC (.BAS) → vmsbasic transpiler → C source → gcc → links against OVMX + libvmsbasic_rtl
```

### Deliverables

1. **BASIC-to-C transpiler** (`tools/vmsbasic/`)
   - BASIC-PLUS-2 parser (recursive descent)
   - C code generation targeting OVMX APIs
   - Key mappings:
     - `WHEN ERROR` / `HANDLER` → setjmp/longjmp + CHF
     - `OPEN "file" FOR INPUT AS FILE #1` → RMS sys$open / sys$connect
     - `GET #1` / `PUT #1` → sys$get / sys$put
     - `MAP` statements → C struct definitions
     - `FIELD #1` → RMS record buffer layout
     - String variables → VMS dynamic descriptors

2. **BASIC runtime library** (`src/vmsbasic_rtl/`)
   - Channel management (file #N mapping)
   - String operations (BASIC string semantics)
   - PRINT formatting (PRINT USING, TAB, etc.)
   - Math functions (BASIC-specific: INT, FIX, CINT, etc.)

### Validation
- Classic DEC BASIC programs (file I/O, error handling, string manipulation) transpile and run

---

## Phase 13: DECnet Phase IV + BLISS

**Goal**: DECnet networking + BLISS compilation.

**Scope**: Large (8-12 weeks) | **Depends on**: Phase 10 (sys$qio + process management proven)

### DECnet Strategy
Userspace NSP stack with DECnet-over-IP transport. No kernel module initially.

```
VMS C program (sys$qio to NET: device)
    → libvms (routes to DECnet library)
    → libdecnet (NSP, session control, routing)
    → DECnet-over-IP (UDP encapsulation)
    → Linux TCP/IP stack
```

### DECnet Deliverables

1. **NSP implementation** (`src/decnet/nsp/`)
   - Logical link: connect, send, receive, close
   - Flow control, error recovery
   - Reference: PyDECnet (Python, by Paul Koning — https://github.com/pkoning2/pydecnet)

2. **Session control + routing** (`src/decnet/`)
   - Endnode mode (no full routing — forward to designated router)
   - Area.node addressing
   - DECnet-over-IP encapsulation (UDP port 4711, Multinet protocol)

3. **NET: device driver** in OVMX
   - `sys$assign("NET:", &channel)`
   - `sys$qio` with IO$_ACCESS (connect), IO$_READVBLK/WRITEVBLK (data), IO$_DEACCESS (close)
   - Network function codes and modifiers

4. **FAL (File Access Listener)**
   - DAP (Data Access Protocol) client — enables `NODE::DISK:[DIR]FILE` syntax
   - RMS integration: parse NODE:: prefix in file specs, initiate DAP session
   - Logical name integration: `DEFINE REMOTE BOSTON::DUA0:` → transparent remote access

5. **Configuration** — Node name/number, designated router, DECnet-over-IP endpoints

### DECnet Validation
- Two OVMX instances communicate via DECnet-over-IP
- `COPY REMOTE::file local_file` works
- Connect to HECnet (real VMS systems) — ultimate validation

### BLISS Deliverables

1. **Adopt blissc** — BSD-licensed LLVM-based BLISS compiler (https://github.com/madisongh/blissc)
   - Git submodule or vendored dependency
   - CMake external project integration

2. **BLISS-to-OVMX bindings** — REQUIRE files defining VMS structures for BLISS programs

3. **Validation** — Small BLISS programs calling sys$ services compile and run

### Legal
- DECnet specs are public, no patents, no licensing required
- Use "DECnet Phase IV compatible" (trademark awareness)
- Implement from published specs (clean room)

---

## Agent Team

### Current Agents (role shifts)

| Agent | Current Focus | New Focus |
|-------|--------------|-----------|
| **Systems Engineer** | General implementation | Phase 8-10: sys$fao, CHF, CRTL, CLI$ routines, sys$qio network device |
| **QA Engineer** | Test infrastructure | All phases: conformance harness, corpus management, CI/CD for automated compatibility testing |
| **Technical Writer** | API docs | Phases 9-11: CRTL documentation, VMS-to-OVMX porting guide, Fortran porting guide |
| **Blog** | Devblog posts | Ongoing: compatibility milestones are excellent blog material |

### New Agents

| Agent | Spec | Role | Phases |
|-------|------|------|--------|
| **Compatibility Engineer** | `docs/agent-compat.md` | Audits OVMX headers/APIs against real OpenVMS Alpha. Writes conformance tests. Identifies gaps. Validates fixes against corpus. | 8, 9, 10 |
| **Compiler Engineer** | `docs/agent-compiler.md` | Builds Fortran preprocessor, BASIC transpiler, BLISS integration. Language parsing and code generation. | 11, 12, 13 |
| **Network Engineer** | `docs/agent-network.md` | Implements DECnet NSP stack, FAL, DAP. Tests against HECnet. | 13 |

### Model Routing (new task types)

| Task Type | Model |
|-----------|-------|
| Header audit (OVMX vs real VMS) | Sonnet |
| CHF architecture (stack unwinding, signal safety, thread safety) | Opus |
| CRTL shim design (stdio interception strategy) | Opus |
| sys$fao, sys$getmsg implementation | Sonnet |
| Conformance test writing | Sonnet |
| Fortran preprocessor parser | Opus |
| BASIC transpiler | Sonnet |
| DECnet protocol implementation from specs | Opus |
| Mechanical header/constant additions | Haiku |

---

## Current Backlog Disposition

The existing 14 beads get reprioritized against source compatibility:

| Bead | Current | Disposition |
|------|---------|-------------|
| vms-jh8: Split dcl_builtin.c | P2 | **Defer** — cosmetic, doesn't affect compatibility |
| vms-t1g: CI/CD pipeline | P2 | **Keep P2** — needed for conformance test automation |
| vms-di1: Static analysis | P2 | **Defer** — nice but doesn't close API gaps |
| vms-a5m: sys$ API docs | P2 | **Defer** — docs follow implementation, not before |
| vms-r1v: RTL API docs | P2 | **Defer** |
| vms-0fl: DCL command reference | P2 | **Defer** |
| vms-hhu: Userspace test coverage | P3 | **Subsume into Phase 8** — conformance harness replaces this |
| vms-cww: Config file docs | P3 | **Defer** |
| vms-6as: SYSUAF admin guide | P3 | **Defer** |
| vms-h6e: DCL pipes/redirection | P4 | **Promote to P2** — GNV tools (Phase 10+) will need pipes |
| vms-p8o: HELP content database | P4 | **Defer** |
| vms-5yk: Buildroot integration | P4 | **Defer** |
| vms-zwq: Rightslist integration | P4 | **Defer** |
| vms-e9x: FUSE ODS-2 driver | P4 | **Defer** |

New beads to create for the roadmap phases will be created after plan approval.

---

## Risk Register

1. **Header binary incompatibility** — If OVMX struct layouts differ from real VMS Alpha (padding, field order, sizes), programs corrupt data silently. Mitigation: Phase 9 audit is critical; get real OpenVMS Alpha headers from HP/VSI hobbyist kit.

2. **CHF complexity** — Condition handling with stack unwinding interacts with threads, signals, longjmp. Mitigation: implement common cases first (lib$establish + lib$signal + RESIGNAL/CONTINUE), defer sys$unwind to later.

3. **CRTL scope creep** — VMS DECC RTL has hundreds of functions. Mitigation: implement only what the corpus requires, driven by conformance test failures.

4. **Test corpus access** — Eight-Cubed site may go down, some code may have unclear licensing. Mitigation: archive locally, document provenance.

5. **DECnet protocol complexity** — Full DNA stack is substantial. Mitigation: endnode mode only, DECnet-over-IP only, minimal feature set (NSP + FAL).

---

## Verification

Each phase has a concrete validation target:

| Phase | Validation |
|-------|-----------|
| 8 | 50+ Eight-Cubed examples compile and run |
| 9 | Eight-Cubed stdio programs work; simpler MMK files compile |
| 10 | MMK compiles, links, and processes an MMS file |
| 11 | VMS Fortran programs compile with GFortran + preprocessor |
| 12 | DEC BASIC programs transpile to C and run |
| 13 | Two OVMX nodes communicate via DECnet; BLISS programs compile |
