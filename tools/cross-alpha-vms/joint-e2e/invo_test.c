/* Anchorless "return to main" through SYS$UNWIND on OVMX/Alpha (vms-ed1).
 *
 * main establishes a condition handler and calls f(), which calls g(), which
 * signals. The handler sets the mechanism array's saved R0 to 42 and asks
 * SYS$UNWIND to unwind to main's establisher depth. main armed NO resume
 * anchor (no setjmp): the only way back into main is the genuine Calling-
 * Standard path -- LIB$GET_CURR_INVO_CONTEXT captures the live registers, the
 * PDSC/RSA walk (procedure descriptor found through each frame's FP) climbs
 * dispatcher -> lib$signal -> g -> f -> main, restoring every preserved
 * register, and the machine transfer resumes main at the instruction after
 * its call to f() with R0 = 42. So f() "returns" 42, g() never returns, and
 * main's own locals (held in preserved registers / its frame) are intact.
 * Sentinel 7 = all of that held. */
extern int printf(const char *, ...);
extern void *lib$establish(void *);
extern unsigned int lib$signal(unsigned int, ...);
extern unsigned int sys$unwind(const unsigned int *, void *);

struct ovmx_mech {               /* chfdef.h chf$mech_array, 64-bit pointers */
    unsigned int args, flags;
    void        *frame;
    unsigned int depth, savr0, savr1;
};

static int handler_ran, g_returned, f_returned;

static unsigned int handler(unsigned int *sig, struct ovmx_mech *m)
{
    (void)sig;
    handler_ran = 1;
    unsigned int depth = m->depth;   /* main's establisher depth */
    m->savr0 = 42;                   /* what main's call to f() will "return" */
    sys$unwind(&depth, 0);
    return 1;
}

static int g(void)
{
    lib$signal(0x2C, 0);              /* SS$_ABORT (severe) */
    g_returned = 1;
    return 5;
}

static int f(void)
{
    int r = g();
    f_returned = 1;
    return r + 100;
}

int main(int argc, char **argv, char **envp)
{
    (void)argv; (void)envp;
    volatile int keep = 1234;          /* a local that must survive the unwind */
    lib$establish((void *)handler);
    int r = f();
    printf("OVMX invo test: f() -> %d handler=%d g_returned=%d f_returned=%d keep=%d argc=%d\n",
           r, handler_ran, g_returned, f_returned, keep, argc);
    if (r == 42 && handler_ran && !g_returned && !f_returned && keep == 1234)
        return 7;   /* distinctive success -> $STATUS = C$_EXIT1 + (7-1)*8 */
    return 3;
}
