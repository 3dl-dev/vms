# DEC C exit() and __posix_exit() completion status on OpenVMS Alpha V8.4 (2026-10-21)

Evidence for rd vms-599. Captured on a disposable lab pod (`exitalpha-1`, node `ALPHA1`,
OpenVMS Alpha V8.4 on AXPbox), driven over the console FIFO with `tools/lab-alpha/`.
The pod and its lab directory were deleted afterwards.

The node has `MACRO` and `LINK` but no C compiler. Each probe is our own MACRO-32
program, which calls the C RTL entry point directly with one value:
`PUSHL #n` / `CALLS #1,G^DECC$EXIT` (or `DECC$__POSIX_EXIT`). The program is then
`MACRO`ed, `LINK`ed and `RUN`, and `F$STRING($STATUS)` is printed (`CONSOLE.txt`).

| call | `$STATUS` |
|---|---|
| `DECC$EXIT(0)` | `%X00000001` |
| `DECC$EXIT(1)` | `%X00000001` |
| `DECC$EXIT(2)` | `%X00000002` (DCL: `%NONAME-E-NOMSG`) |
| `DECC$EXIT(3)` | `%X00000003` |
| `DECC$EXIT(4)` | `%X00000004` (DCL: `%NONAME-F-NOMSG`) |
| `DECC$EXIT(%X1000002C)` | `%X1000002C` |
| `DECC$__POSIX_EXIT(0)` | `%X00000001` |
| `DECC$__POSIX_EXIT(1)` | `%X1035A00A` |
| `DECC$__POSIX_EXIT(2)` | `%X0035A011` |
| `DECC$__POSIX_EXIT(3)` | `%X0035A019` |
| `DECC$__POSIX_EXIT(255)` | `%X0035A7F9` |
| `DECC$__POSIX_EXIT(256)` | `%X00000001` |

So `exit()` turns 0 into SS$_NORMAL and passes every other value through unchanged.
`__posix_exit()` keeps the low eight bits. It turns 0 into `%X00000001` and turns any
other n into `%X0035A001 + (n << 3)`, a C-facility message with success severity.
The one exception is 1: it becomes an error with the message inhibited, `%X1035A00A`.

`EXIT_FAILURE` is a header constant and cannot be observed without a compiler. The VSI
C RTL Reference (exit) groups "a status value of 2 or EXIT_FAILURE" as the error-level
value, which matches the `%X00000002` measured above.

Clean-room: observation only. Nothing was disassembled, and no VSI/HPE source or header
was read.
