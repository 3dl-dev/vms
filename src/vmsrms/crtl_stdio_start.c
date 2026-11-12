/*
 * crtl_stdio_start.c - a statically linked OVMX image's standard streams are
 * SYS$INPUT, SYS$OUTPUT and SYS$ERROR from image start-up (rd vms-07ae).
 *
 * WHAT VMS DOES. Every C image's stdin, stdout and stderr are the process's
 * SYS$INPUT, SYS$OUTPUT and SYS$ERROR, opened by the C RTL before main() (VSI
 * C Run-Time Library Reference Manual, "Standard I/O": stdin/stdout/stderr are
 * the process-permanent files). A DECC$SHR image gets that from the C RTL's
 * image entry (decc$main on Alpha, the activator's call of ovmx_crtl_fd_install
 * on x86_64, crtl_rms_fd.c). A STATIC utility image (SYSGEN, AUTHORIZE,
 * INSTALL...) has no DECC$SHR and no decc$main: until now its stdin was
 * whatever the substrate left on descriptor 0.
 *
 * WHAT THIS DOES. Linked into every static image of a build whose C RTL base
 * has the system-call funnel (src/crtl-musl-x86_64; top-level CMakeLists.txt,
 * OVMX_CRTL_STDIO_START), its constructor binds descriptors 0, 1 and 2 through
 * the C RTL device layer -- the SAME ovmx_crtl_devfd_bind_stdio the DECC$SHR
 * images run (crtl_devfd.c): a terminal or a mailbox SYS$ stream becomes a
 * channel $ASSIGNed to it, served by $QIO; a file, or a stream the substrate
 * has redirected underneath the VMS name, is left as it is (crtl_devfd.c head
 * comment). It installs the device layer alone: the static image's files stay
 * where its own code puts them (the RMS file layer is a static image's to turn
 * on, ovmx_crtl_fd_install, as tests/qemu/corpus_rt_start.c does).
 *
 * NO EXECUTIVE. $ASSIGN of SYS$INPUT fails (SS$_NOSUCHDEV, no /dev/vms) and the
 * stream is not bound: descriptor 0/1/2 stay the substrate's, exactly as a
 * DECC$SHR image's do in the same case (ovmx_crtl_devfd_bind_stdio). When no
 * stream is bound the hook is not installed at all.
 *
 * An image that turns the full C RTL file layer on (ovmx_crtl_fd_install) takes
 * the hook over; the streams bound here stay bound (bind_stdio skips them).
 */
#include <errno.h>
#include <sys/syscall.h>

typedef long long (*ovmx_sys_hook_fn)(long long, long long, long long, long long,
                                      long long, long long, long long, int *);
extern ovmx_sys_hook_fn __ovmx_sys_hook;            /* the musl funnel's cell */

extern void ovmx_crtl_devfd_bind_stdio(void);       /* crtl_devfd.c */
extern int ovmx_crtl_devfd_is(long long fd);
extern void ovmx_crtl_devfd_rundown(void);
extern long long ovmx_crtl_devfd_call(long long, long long, long long, long long,
                                      long long, long long, int *);
/* LIBVMS: nonzero while a system service does the substrate's own work. */
extern int ovmx_substrate_active(void);

/* The device layer takes the system services WEAK (crtl_devfd.c, DEVFD_SVC), so
 * that an image which links no LIBVMS carries none. This start-up is linked only
 * into images that link LIBVMS (CMakeLists.txt), and names each service the
 * layer calls strongly, so the static link pulls them in: without them the
 * layer would be silently absent. */
extern unsigned int sys$assign(), sys$dassgn(), sys$crembx(), sys$qiow(),
                    sys$getdviw(), sys$trnlnm();
__attribute__((used))
static const void *const crtl_stdio_services[] = {
    (const void *)sys$assign, (const void *)sys$dassgn, (const void *)sys$crembx,
    (const void *)sys$qiow, (const void *)sys$getdviw, (const void *)sys$trnlnm,
    (const void *)ovmx_substrate_active,
};

/* crtl_devfd.c releases an RMS file a dup2 lands on. With only the device
 * layer there are no RMS files of the C RTL's; an image that also links the
 * RMS file layer (crtl_rms_fd.c) supplies the real one, which wins. */
__attribute__((__weak__)) void ovmx_crtl_rms_unbind(long long fd) { (void)fd; }

static int busy;    /* the device layer's own system calls are the kernel's */

static long long stdio_hook(long long n, long long a1, long long a2, long long a3,
                            long long a4, long long a5, long long a6, int *handled)
{
    (void)a6;
    if (busy || ovmx_substrate_active())
        return 0;
    busy = 1;
    int saved = errno;
    long long r = 0;
    if (n == SYS_exit_group)
        ovmx_crtl_devfd_rundown();              /* a pipe's mailbox ends */
    else
        r = ovmx_crtl_devfd_call(n, a1, a2, a3, a4, a5, handled);
    errno = saved;
    busy = 0;
    return r;
}

void ovmx_crtl_stdio_install(void)
{
    if (__ovmx_sys_hook)
        return;                                 /* a C RTL layer is already on */
    busy = 1;
    ovmx_crtl_devfd_bind_stdio();
    busy = 0;
    if (ovmx_crtl_devfd_is(0) || ovmx_crtl_devfd_is(1) || ovmx_crtl_devfd_is(2))
        __ovmx_sys_hook = stdio_hook;
}

/* Before any constructor of the image's own: its stdio is SYS$'s from the start. */
__attribute__((constructor(101)))
static void ovmx_crtl_stdio_start(void)
{
    ovmx_crtl_stdio_install();
}
