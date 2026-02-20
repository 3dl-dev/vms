# DCL Pipes and I/O Redirection — Design Document

**Bead**: vms-h6e
**Status**: Design and decomposition phase
**Date**: 2026-02-19

---

## 1. Current State of DCL I/O

### I/O Model

DCL I/O in OVMX is currently wired directly to the process stdio streams:

- `SYS$INPUT` — reads from `stdin` (or the current command procedure `FILE *fp`)
- `SYS$OUTPUT` — writes via `printf()` / `fputs(stdout, ...)`
- `SYS$ERROR` — writes via `fprintf(stderr, ...)`

Key files:

| File | Role |
|------|------|
| `src/vmsdcl/dcl_io.c` | `dcl_read_input()`, `dcl_write_output()`, `dcl_write_error()` |
| `src/vmsdcl/dcl_builtin.c` | `cmd_write()` checks channel name for `SYS$OUTPUT`/`SYS$ERROR` |
| `src/vmsdcl/dcl_filespec.c` | `dcl_translate_logical()` — hard-wired translations for known logicals |
| `src/vmsdcl/dcl_main.c` | Shell main loop calling `dcl_read_input()` |

### What Already Exists

The lexer (dcl_lexer.c) **already tokenizes `|`** as `TOK_PIPE`. The parser (dcl_parser.c) handles `TOK_PIPE` by calling `collect_rest()` into `cmd->rest` — so the pipe character is recognized but not acted upon.

There is a stub `cmd_pipe()` in `dcl_builtin.c` (line 1545). It currently takes all params and the `rest` field, concatenates them with ` | `, and calls `system()`. This is a Unix-shell escape, not VMS PIPE semantics.

The `cmd_define()` implementation stores logical names as DCL symbols (not as actual LNM entries). There is no mechanism today to redirect `SYS$OUTPUT` by redefining a logical name — `dcl_translate_logical()` does not consult the symbol table; it handles only a fixed set of well-known names.

The `dcl_context_t` has an `open file channels` array (16 slots) for `OPEN`/`CLOSE`/`READ`/`WRITE`. This is per-channel file I/O, separate from the process-level `SYS$OUTPUT` stream.

`sys$crembx` and `sys$delmbx` are implemented in `src/libvms/syssvc/sys_mailbox.c` using Unix `socketpair(AF_UNIX, SOCK_DGRAM)`. The channel table in the PCB holds both ends of the pair.

`sys$creprc` is declared in `starlet.h` but its implementation is in `src/libvms/syssvc/sys_process.c` — this is the VMS process creation service.

---

## 2. VMS PIPE Command Behavior

### History and Semantics

The VMS `PIPE` command was introduced in OpenVMS 7.1. It is a DCL built-in verb — not a separate utility — that establishes a pipeline of DCL commands. It is **not** a Unix-style pipe; it uses VMS mailboxes (MBA<n>:) as the pipe conduit between subprocesses.

### Syntax

```
PIPE cmd1 | cmd2 | cmd3
PIPE cmd1 && cmd2            ! sequential, second runs only if first succeeds
PIPE cmd1 || cmd2            ! sequential, second runs only if first fails
PIPE cmd1 ; cmd2             ! sequential, second always runs
PIPE (cmd1 | cmd2) && cmd3   ! grouped
```

The `|` inside a PIPE command creates a **named mailbox pipe** between adjacent commands. Each command in the pipeline runs as a **DCL subprocess** (via `sys$creprc`) with:
- Its `SYS$INPUT` redefined to the read end of the preceding mailbox
- Its `SYS$OUTPUT` redefined to the write end of the following mailbox

The outermost `SYS$INPUT` / `SYS$OUTPUT` of the PIPE command itself are inherited from the parent process.

### File Redirection

VMS does not have a `>` redirection operator in the traditional Unix sense. Instead, redirection is done by redefining logical names before the command:

```dcl
! Redirect output to a file
DEFINE/USER_MODE SYS$OUTPUT output.txt
command-to-run

! Redirect input from a file
DEFINE/USER_MODE SYS$INPUT input.txt
command-to-run

! Combined output redirect inside PIPE (common idiom):
PIPE command > output.txt        ! VMS 7.2+ shorthand using PIPE redirection
PIPE command /OUTPUT=output.txt  ! Some commands have /OUTPUT qualifier
```

The `DEFINE/USER_MODE` qualifier is critical: it creates a logical name that persists only for the duration of the next image activation — it is automatically deleted after one use. This is the VMS mechanism for per-command I/O redirection.

VMS also supports `>>` (append) in PIPE context (OpenVMS 7.2+).

### Key VMS Behaviors to Match

1. `PIPE` is a command verb, not a shell meta-character. Outside of a PIPE command, `|` has no meaning.
2. Each pipeline stage runs as a subprocess with mailbox stdio — not as a fork/exec in the Unix sense.
3. `&&`, `||`, `;` are **compound statement separators** within PIPE, not general DCL operators.
4. `DEFINE/USER_MODE SYS$OUTPUT file.txt` redirects output for exactly one subsequent command.
5. Error output (SYS$ERROR) is not redirected by `>` — it stays on the terminal unless explicitly redirected.
6. `PIPE` propagates `$STATUS` from the last command in the pipeline.

---

## 3. I/O Redirection Design for OVMX

### The Core Gap

The current `dcl_translate_logical()` is a static lookup that ignores any user-defined logical names. There is no per-command or per-process logical name table that `dcl_read_input()` and `dcl_write_output()` consult. To support redirection, we need:

1. A writable process-level logical name table that DCL commands can update.
2. `dcl_read_input()` and `dcl_write_output()` must consult `SYS$INPUT` / `SYS$OUTPUT` in that table to determine actual I/O targets.
3. A scoped "user-mode" logical entry that is consumed after one command execution.

### Recommended Approach: Unix FDs + VMS Syntax

**Principle**: Use Unix file descriptors and pipes internally; present VMS PIPE command syntax externally.

This is the correct pragmatic trade-off for OVMX:
- VMS PIPE creates subprocess pairs connected via mailboxes. OVMX has `sys$crembx` and `sys$creprc` stubs, but wiring them into a full IPC pipeline is complex.
- Using Unix `pipe(2)` + `fork(2)` achieves identical semantics from the user's perspective, at a fraction of the complexity.
- The VMS mailbox API (`sys$crembx`) can be used for other inter-process coordination where byte-stream pipes are insufficient.

**Implementation model**:

```
PIPE cmd1 | cmd2 | cmd3
     ↓
For N commands in pipeline:
  pipe(fd[0], fd[1])      // Create N-1 Unix pipes
  fork() for each stage
  dup2(fd_read, STDIN_FILENO)   // Redirect stdin
  dup2(fd_write, STDOUT_FILENO) // Redirect stdout
  exec DCL command in child
  wait() in parent for all stages
  propagate $STATUS from last stage
```

**For I/O redirection (DEFINE/USER_MODE)**:

```
DEFINE/USER_MODE SYS$OUTPUT file.txt
  → Store "user_mode" entry in dcl_context_t.io_override
  → Next dcl_execute_line() call:
      - Open file.txt, dup2(fileno, STDOUT_FILENO) in child
      - Execute command in subprocess
      - Restore STDOUT_FILENO in parent
      - Clear the user_mode override
```

### DCL Context Changes Needed

The `dcl_context_t` needs an `io_override` struct:

```c
struct dcl_io_override {
    char sys_output[512];    /* NULL = no override, else filename or MBA<n>: */
    char sys_input[512];
    char sys_error[512];
    int  user_mode;          /* 1 = clear after next command */
};
```

The `DEFINE` command needs to recognize `/USER_MODE` qualifier and populate this struct rather than calling `dcl_sym_set()`.

### PIPE Compound Operators

Within a PIPE command:

| Operator | VMS Semantics |
|----------|--------------|
| `\|` | Pipe stdout of left to stdin of right |
| `&&` | Run right only if left exits with success (odd status) |
| `\|\|` | Run right only if left exits with failure (even status) |
| `;` | Run right unconditionally |

These are parsed within the PIPE command string, not by the top-level DCL parser. The PIPE executor gets the full command string after `PIPE` and tokenizes it on `|`, `&&`, `||`, `;`.

---

## 4. What Must NOT Be Changed

- The lexer's `TOK_PIPE` recognition — already correct.
- The `sys$crembx` / `sys$delmbx` implementation — correct and available for future use.
- The `dcl_context_t.channels[]` array — this is for `OPEN`/`READ`/`WRITE` file channels, orthogonal to pipe I/O.
- The `cmd_pipe()` stub location in `dcl_builtin.c` — will be replaced with real implementation.

---

## 5. Implementation Phases (Bead Tree)

The work decomposes into four sequential phases:

### Phase 1 — DEFINE/USER_MODE I/O Redirection

**Deliverable**: `DEFINE/USER_MODE SYS$OUTPUT filename` redirects stdout for the next command. `DEFINE/USER_MODE SYS$INPUT filename` redirects stdin.

**Scope**:
- Add `dcl_io_override_t` struct to `dcl_context_t`
- Modify `cmd_define()` to detect `/USER_MODE` qualifier and set the override struct
- Modify `dcl_execute_line()` / command dispatch to fork a child when an override is set, apply `dup2()` for the redirected FDs, then execute the command in the child and restore in parent
- Clear override after execution (user-mode = one-shot)
- Support both file output redirect and file input redirect
- Support `/USER_MODE SYS$ERROR` as well

**Files**: `dcl_builtin.c`, `dcl_exec.c`, `dcl_io.c`, `dcl_context.h`

### Phase 2 — PIPE Command with `|` Operator

**Deliverable**: `PIPE cmd1 | cmd2` works. Output of cmd1 is piped to stdin of cmd2.

**Scope**:
- Replace the `cmd_pipe()` stub with a real implementation
- Parse the PIPE command argument string on `|` boundaries
- For each `|`-delimited segment, `fork()` + `dup2()` the pipe FDs
- Execute each DCL command segment in a child subprocess
- Wait for all children; propagate last child's status to `$STATUS`
- Support pipelines of 2+ stages

**Files**: `dcl_builtin.c`

### Phase 3 — PIPE Compound Operators (`&&`, `||`, `;`)

**Deliverable**: `PIPE cmd1 && cmd2`, `PIPE cmd1 || cmd2`, `PIPE cmd1 ; cmd2` all work with correct conditional execution semantics.

**Scope**:
- Extend the PIPE command parser to recognize `&&`, `||`, `;` in addition to `|`
- Build a small execution tree or sequential dispatch for compound PIPE
- Implement VMS conditional-execution logic: odd status = success for `&&`; even status = success for `||`
- Compound operators and pipe `|` may be mixed: `PIPE cmd1 | cmd2 && cmd3`

**Files**: `dcl_builtin.c`

### Phase 4 — Integration Tests

**Deliverable**: Test suite covering all pipe and redirection scenarios; CI passes.

**Scope**:
- Add test cases to `tests/integration/` covering:
  - `DEFINE/USER_MODE SYS$OUTPUT` to file and verify file contents
  - `DEFINE/USER_MODE SYS$INPUT` from file and verify command reads from it
  - `PIPE cmd | cmd` two-stage pipeline, stdout flows correctly
  - `PIPE cmd | cmd | cmd` three-stage pipeline
  - `PIPE cmd && cmd` — second runs only on success
  - `PIPE cmd || cmd` — second runs only on failure
  - `PIPE cmd ; cmd` — second always runs
  - Error conditions: bad file for redirect, failed pipe stage
- Update CI workflow if needed

**Files**: `tests/integration/`

---

## 6. Design Decisions and Trade-offs

### VMS Mailboxes vs Unix Pipes

| Approach | Pro | Con |
|----------|-----|-----|
| VMS mailboxes (sys$crembx + sys$creprc) | Authentic VMS behavior; reuses existing mbx impl | Complex process coordination; subprocess must run full DCL shell; heavy for interactive use |
| Unix pipe(2) + fork(2) | Simple; reliable; identical user-visible semantics | Not using VMS APIs for pipe conduit; SYS$INPUT/SYS$OUTPUT are not real mailbox device names in process table |

**Decision: Unix pipe(2) + fork(2) for the conduit, VMS syntax on top.**

Rationale: Users issue `PIPE TYPE foo.txt | SEARCH SYS$PIPE "error"` — they do not care whether the conduit is a socketpair or a `pipe(2)`. The VMS mailbox API adds significant complexity (assigning channels, converting FDs, managing MBA<n>: device names across fork) with no visible benefit at this stage. The mailbox infrastructure can be engaged in a future phase if VMS-compatible IPC between independently-started processes is needed.

### DEFINE/USER_MODE vs Shell Redirection Syntax

VMS does not have `> file` at the command line level outside of a PIPE context. We should not add it to the top-level DCL parser — that would break VMS compatibility. The correct path is `DEFINE/USER_MODE SYS$OUTPUT file.txt` for standalone redirect, and `PIPE cmd > file` (which VMS 7.2 supports as PIPE-context redirection) as a convenience.

Phase 1 implements the `DEFINE/USER_MODE` mechanism. Phase 2 can optionally add `>` / `>>` recognition within the PIPE argument string (that is contained scope, not top-level DCL syntax).

### $STATUS Propagation

VMS PIPE propagates `$STATUS` from the **last** command in the pipeline. For `&&` and `||` chains, `$STATUS` is the status of the last command that actually executed. This matches bash behavior for `$?` and is straightforward to implement.

### SYS$PIPE Logical

On real VMS, `SYS$PIPE` is the logical name that refers to the mailbox pipe connecting pipeline stages (i.e., a process inside a PIPE reads from `SYS$PIPE`). In Phase 2, when a child is executing inside a PIPE stage, we should define `SYS$PIPE` in its process context pointing to its stdin (which is the pipe FD). This is required for commands like `SEARCH SYS$PIPE "pattern"` to work correctly inside a pipeline.

---

## 7. File Impact Summary

| File | Change |
|------|--------|
| `src/vmsdcl/include/dcl/context.h` | Add `dcl_io_override_t` struct and field to `dcl_context_t` |
| `src/vmsdcl/dcl_io.c` | Update `dcl_read_input()`, `dcl_write_output()` to consult override |
| `src/vmsdcl/dcl_exec.c` | Apply fork/dup2 when override is set before command dispatch |
| `src/vmsdcl/dcl_builtin.c` | Extend `cmd_define()` for `/USER_MODE`; replace `cmd_pipe()` stub |
| `src/vmsdcl/dcl_filespec.c` | Extend `dcl_translate_logical()` to consult context override |
| `tests/integration/` | New test programs for redirection and pipe scenarios |
