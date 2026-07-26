/*
 * test_lib.c — proof shareable image for IMGACT.EXE (bead vms-913.2).
 *
 * Built as LIBTEST$SHR.EXE (musl -shared, -mtls-dialect=desc, sysv hash).
 * Deliberately exercises every relocation type IMGACT must handle:
 *
 *   R_AARCH64_RELATIVE  -> `lp` points at module-local data (base-relative).
 *   R_AARCH64_GLOB_DAT  -> `imgact_counter` (interpreter-provided) via GOT.
 *   R_AARCH64_JUMP_SLOT -> call to `imgact_probe` (interpreter-provided).
 *   R_AARCH64_TLSDESC   -> `__thread tls_x` accessed global-dynamic.
 *
 * Freestanding: no libc dependency, so IMGACT's activation mechanism is
 * isolated from libc-provider concerns (that is build-integration, bead 913.3).
 */

/* TLS variable -> TLSDESC relocation + TLS block setup. */
__thread int tls_x = 42;

/* Module-local data referenced through a pointer -> RELATIVE relocation. */
static int local_seven = 7;
int *lp = &local_seven;

/* Symbols resolved against IMGACT.EXE itself (interpreter as symbol root). */
extern int imgact_probe(void);   /* -> JUMP_SLOT to interpreter builtin */
extern int imgact_counter;       /* -> GLOB_DAT  to interpreter builtin */

int lib_add(int a, int b)   { return a + b + imgact_probe(); }
int get_tls_x(void)         { return tls_x; }
int get_lp(void)            { return *lp; }
int read_counter(void)      { return imgact_counter; }
