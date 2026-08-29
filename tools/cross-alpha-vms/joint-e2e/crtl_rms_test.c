/* crtl_rms_test.c — a richer alpha-dec-vms CRTL/RMS port program, wired as a
 * REPRODUCIBLE joint-e2e VARIANT (build-joint-image.sh's JOINT_MAIN override).
 *
 * Unlike joint_main.c (the P1 milestone crt0-activation smoke test — printf +
 * a distinctive return, kept intact), this program exercises a GENUINE DECC$
 * CRTL working set on the real executive: heap (malloc/free), RMS file I/O via
 * the C stdio stream layer (fopen/fwrite/fread/fclose), and stdio output
 * (printf + fprintf(stderr,...)), with a memset fill and a memcmp read-back
 * verify. It is compiled by the REAL alpha-dec-vms cross cc1 and links
 * zero-deferred against the GENUINE alpha DECC$SHR: every reference here
 * (malloc/free/fopen/fwrite/fread/fclose/printf/fprintf/memset/memcmp and the
 * stderr DATA universal) binds to DECC$SHR — the compiler decorates both this
 * TU's references and musl-alpha's own definitions identically, and
 * mk_decc_shr.sh exports the whole decc$ surface (stderr/stdout/stdin as DATA
 * universals). No POSIX bypass, no fake success: this is the shipping
 * alpha-dec-vms PORT path (vms-da0 ladder), a real CRTL/RMS program.
 *
 * SENTINEL-RETURN CONVENTION (deterministic — no argv dependence; main returns
 * a constant on the success path). crt0 maps the return N through C$_EXIT1, so
 * BOOT-B decodes $STATUS as C$_EXIT1 + (N-1)*8, telling us exactly how far the
 * CRTL/RMS round-trip got:
 *
 *   7  = FULL SUCCESS  (heap + write + read + pattern verified + stdio)
 *   1  = malloc(write buffer) failed
 *   2  = fopen(write) failed
 *   3  = fwrite short/failed
 *   4  = fopen(read) failed
 *   5  = malloc(read buffer) failed
 *   6  = fread short/failed
 *   8  = memcmp mismatch (data corruption on the RMS round-trip)
 */

/* alpha-dec-vms is LP64 (-mpointer-size=64): long/pointer are 64-bit. Match
 * joint_main.c's extern-declaration style (no libc headers are set up in the
 * cross image); the symbol NAMES are what matter for the link, and the cross
 * cc1 decorates them to the decc$ surface at codegen. */
typedef unsigned long ovmx_size_t;

extern void *malloc(ovmx_size_t);
extern void  free(void *);
extern void *memset(void *, int, ovmx_size_t);
extern int   memcmp(const void *, const void *, ovmx_size_t);

extern void      *fopen(const char *, const char *);
extern ovmx_size_t fwrite(const void *, ovmx_size_t, ovmx_size_t, void *);
extern ovmx_size_t fread(void *, ovmx_size_t, ovmx_size_t, void *);
extern int         fclose(void *);

extern int   printf(const char *, ...);
extern int   fprintf(void *, const char *, ...);
extern void *stderr;   /* the FILE* stream DATA universal exported by DECC$SHR */

/* vms-430 PC discriminator (DIAGNOSTIC, not for merge): raw MAIN-DISC brackets.
 * write() is the DEC C RTL decc$write universal (musl-alpha's write, exported by
 * DECC$SHR) — a thin syscall wrapper, NOT the buffered stdio path under suspicion,
 * so a bracket prints even if the CRTL call it precedes is the one that faults.
 * The 3rd arg is size_t (64-bit on this LLP64 target: unsigned long long); the
 * return is ssize_t (ignored). MARK() emits a fixed literal (compile-time length).
 * These land in the JOINT image (JOINT_E2E.EXE), independent of the DECC$SHR
 * handler above. */
extern long long write(int, const void *, unsigned long long);
#define MARK(s) ((void)write(2, (s), sizeof(s) - 1))

/* vms-430 SP discriminator (DIAGNOSTIC): trace the caller's own SP across the
 * CRTL call sequence. Read $30 (SP on Alpha) with inline asm right beside each
 * MAIN-DISC bracket and emit "MAIN-SP: <point> sp=0x<16>". If SP is IDENTICAL
 * at pre-malloc and pre-memset, any imbalance is inherited from the crt0->main
 * handoff (not drifting between calls); if it DRIFTS, main's own codegen is
 * imbalancing the stack; a non-16-byte-aligned SP is itself the tell. Emitted
 * via decc$write (the same thin syscall wrapper as MARK), no stdio. Lands in
 * JOINT_E2E.EXE. __alpha__-guarded so a host -fsyntax-only pass over the shared
 * source needs no Alpha register. */
static void main_sp(const char *point)
{
    unsigned long sp = 0UL;
#if defined(__alpha__)
    __asm__ volatile("mov $30,%0" : "=r"(sp));
#endif
    char b[64];
    char *p = b;
    const char *s = "MAIN-SP: ";
    while (*s) *p++ = *s++;
    while (*point) *p++ = *point++;
    const char *h = " sp=0x";
    while (*h) *p++ = *h++;
    for (int i = 15; i >= 0; i--) {
        unsigned d = (unsigned)((sp >> (i * 4)) & 0xfUL);
        *p++ = (char)(d < 10 ? ('0' + d) : ('a' + (d - 10)));
    }
    *p++ = '\n';
    (void)write(2, b, (unsigned long long)(p - b));
}

#define PT_SIZE   8192          /* KB-scale buffer */
#define PT_NAME   "PORTTEST.DAT"

int main(int argc, char **argv, char **envp)
{
    (void)argv; (void)envp;

    /* vms-430: prove control reached main BEFORE any CRTL call. Its presence
     * splits handoff-crash (absent) from in-main crash (present). */
    MARK("MAIN-DISC: entered\n");
    main_sp("entered");

    /* 1. heap: allocate + fill with a known deterministic pattern. */
    MARK("MAIN-DISC: pre-malloc\n");
    main_sp("pre-malloc");
    unsigned char *buf = (unsigned char *)malloc(PT_SIZE);
    if (!buf)
        return 1;
    MARK("MAIN-DISC: pre-memset\n");
    main_sp("pre-memset");
    memset(buf, 0, PT_SIZE);
    for (int i = 0; i < PT_SIZE; i++)
        buf[i] = (unsigned char)((i * 7 + 3) & 0xFF);

    /* 2. RMS file I/O: create + write the buffer. */
    MARK("MAIN-DISC: pre-fopen-w\n");
    main_sp("pre-fopen-w");
    void *wf = fopen(PT_NAME, "w");
    if (!wf) { free(buf); return 2; }
    MARK("MAIN-DISC: pre-fwrite\n");
    main_sp("pre-fwrite");
    if (fwrite(buf, 1, PT_SIZE, wf) != (ovmx_size_t)PT_SIZE) {
        fclose(wf); free(buf); return 3;
    }
    MARK("MAIN-DISC: pre-fclose-w\n");
    main_sp("pre-fclose-w");
    fclose(wf);

    /* 3. RMS file I/O: reopen + read it back + verify the pattern. */
    MARK("MAIN-DISC: pre-fopen-r\n");
    main_sp("pre-fopen-r");
    void *rf = fopen(PT_NAME, "r");
    if (!rf) { free(buf); return 4; }
    MARK("MAIN-DISC: pre-malloc2\n");
    main_sp("pre-malloc2");
    unsigned char *rbuf = (unsigned char *)malloc(PT_SIZE);
    if (!rbuf) { fclose(rf); free(buf); return 5; }
    MARK("MAIN-DISC: pre-fread\n");
    main_sp("pre-fread");
    if (fread(rbuf, 1, PT_SIZE, rf) != (ovmx_size_t)PT_SIZE) {
        fclose(rf); free(rbuf); free(buf); return 6;
    }
    fclose(rf);

    MARK("MAIN-DISC: pre-memcmp\n");
    main_sp("pre-memcmp");
    if (memcmp(buf, rbuf, PT_SIZE) != 0) {
        free(rbuf); free(buf); return 8;
    }

    /* 4. stdio variety: fprintf to stderr + printf to stdout. */
    MARK("MAIN-DISC: pre-fprintf-stderr\n");
    main_sp("pre-fprintf-stderr");
    fprintf(stderr, "OVMX CRTL/RMS port test: wrote+read %d bytes via '%s', pattern verified\n",
            PT_SIZE, PT_NAME);
    MARK("MAIN-DISC: pre-printf\n");
    main_sp("pre-printf");
    printf("OVMX CRTL/RMS port test: OK (heap+RMS+stdio) argc=%d\n", argc);

    /* 5. release the heap + return the full-success sentinel. */
    free(rbuf);
    free(buf);
    return 7;   /* distinctive success -> $STATUS = C$_EXIT1 + (7-1)*8 */
}
