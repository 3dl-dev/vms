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

#define PT_SIZE   8192          /* KB-scale buffer */
#define PT_NAME   "PORTTEST.DAT"

int main(int argc, char **argv, char **envp)
{
    (void)argv; (void)envp;

    /* 1. heap: allocate + fill with a known deterministic pattern. */
    unsigned char *buf = (unsigned char *)malloc(PT_SIZE);
    if (!buf)
        return 1;
    memset(buf, 0, PT_SIZE);
    for (int i = 0; i < PT_SIZE; i++)
        buf[i] = (unsigned char)((i * 7 + 3) & 0xFF);

    /* 2. RMS file I/O: create + write the buffer. */
    void *wf = fopen(PT_NAME, "w");
    if (!wf) { free(buf); return 2; }
    if (fwrite(buf, 1, PT_SIZE, wf) != (ovmx_size_t)PT_SIZE) {
        fclose(wf); free(buf); return 3;
    }
    fclose(wf);

    /* 3. RMS file I/O: reopen + read it back + verify the pattern. */
    void *rf = fopen(PT_NAME, "r");
    if (!rf) { free(buf); return 4; }
    unsigned char *rbuf = (unsigned char *)malloc(PT_SIZE);
    if (!rbuf) { fclose(rf); free(buf); return 5; }
    if (fread(rbuf, 1, PT_SIZE, rf) != (ovmx_size_t)PT_SIZE) {
        fclose(rf); free(rbuf); free(buf); return 6;
    }
    fclose(rf);

    if (memcmp(buf, rbuf, PT_SIZE) != 0) {
        free(rbuf); free(buf); return 8;
    }

    /* 4. stdio variety: fprintf to stderr + printf to stdout. */
    fprintf(stderr, "OVMX CRTL/RMS port test: wrote+read %d bytes via '%s', pattern verified\n",
            PT_SIZE, PT_NAME);
    printf("OVMX CRTL/RMS port test: OK (heap+RMS+stdio) argc=%d\n", argc);

    /* 5. release the heap + return the full-success sentinel. */
    free(rbuf);
    free(buf);
    return 7;   /* distinctive success -> $STATUS = C$_EXIT1 + (7-1)*8 */
}
