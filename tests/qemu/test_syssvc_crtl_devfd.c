/*
 * test_syssvc_crtl_devfd.c - the C RTL's terminals and pipes are VMS channels,
 * and no name a C program opens reaches the substrate's file system (vms-a66e).
 *
 * On OpenVMS a C program's stdout on a terminal is SYS$OUTPUT, written with
 * $QIO through the terminal driver; its pipe() is a mailbox pair; and
 * open("/proc/self/status") is an RMS file specification RMS cannot find. OVMX's
 * C RTL does each through src/vmsrms/crtl_devfd.c and crtl_rms_fd.c, reached
 * through the musl system-call funnel (src/crtl-musl-x86_64), the same code
 * DECC$SHR runs on OVMX/Alpha.
 *
 * THE DECISIVE SHAPE. The test makes its substrate stdin/stdout a terminal line
 * of its own (a DECOY pty: what a program run from DCL inherits underneath),
 * and points SYS$INPUT / SYS$OUTPUT at a DIFFERENT terminal, an RTAn: unit of
 * the executive's terminal driver. Then printf() must arrive on the RTAn:
 * line and nothing on the decoy; fgets(stdin) must read the line typed on the
 * RTAn:. A C RTL that still wrote its descriptor 1 through the kernel would
 * put the line on the decoy.
 *
 * pipe(): getname() names a mailbox, $GETDVI says DC$_MAILBOX, each write is a
 * message, close of the write end reads as end of file, and the temporary
 * mailbox is gone when both ends are closed. The kernel namespaces /proc,
 * /sys, /run and /dev are RMS names RMS does not have (ENOENT), /dev/null is
 * the null device, and a path relative to a directory descriptor resolves in
 * that RMS directory (a pipe is not a directory: ENOTDIR; no descriptor:
 * EBADF). Inside the substrate scope (a system service's own work) the kernel
 * namespaces are the kernel's.
 *
 * Registered only where the layer is linked (OVMX_CRTL_FD_STATIC; tests/qemu/
 * CMakeLists.txt). No /dev/vms: SKIP.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "starlet.h"
#include "descrip.h"
#include "lnmdef.h"
#include "dvidef.h"
#include "dcdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"
#include "ovmx_vterm.h"
#include "ovmx_substrate.h"

#define EXIT_SKIP 77
#define UNIT "VDA0:"

static FILE *rep;                       /* the report: the harness's own stdout */
static int pass, fail;
static void check(int c, const char *m)
{
    if (c) { fprintf(rep, "  PASS: %s\n", m); pass++; }
    else   { fprintf(rep, "  FAIL: %s\n", m); fail++; }
}

#if defined(OVMX_CRTLFD_STATIC)
void ovmx_crtl_fd_install(void);
char *ovmx_crtl_getname(int fd, char *buf, ...);

/* A static image takes the device layer's system services weakly
 * (crtl_devfd.c): this one uses the layer, so it links them. */
__attribute__((used)) static void *const devfd_services[] = {
    (void *)sys$assign, (void *)sys$dassgn, (void *)sys$crembx,
    (void *)sys$qiow, (void *)sys$getdviw, (void *)sys$trnlnm,
};

static const struct dsc$descriptor_s ptab =
    { 17, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"LNM$PROCESS_TABLE" };

static int deflog(const char *name, const char *val)
{
    struct dsc$descriptor_s nd = { (uint16_t)strlen(name), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)name };
    struct item_list_3 il[2];
    memset(il, 0, sizeof il);
    il[0].buflen = (uint16_t)strlen(val);
    il[0].item_code = LNM$_STRING;
    il[0].bufaddr = (void *)val;
    return (sys$crelnm(NULL, &ptab, &nd, NULL, il) & 1) != 0;
}

/* $GETDVI one longword item by device name; the status. */
static uint32_t dvi(const char *dev, uint16_t code, uint32_t *out)
{
    struct dsc$descriptor_s d = { (uint16_t)strlen(dev), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)dev };
    struct item_list_3 il[2];
    memset(il, 0, sizeof il);
    il[0].buflen = sizeof *out;
    il[0].item_code = code;
    il[0].bufaddr = out;
    return sys$getdviw(0, 0, &d, il, NULL, NULL, 0, 0);
}

/* What arrives on a pty master within ms (all of it, up to cap). */
static size_t drain(int fd, char *buf, size_t cap, int ms, const char *want)
{
    size_t got = 0;
    while (got < cap - 1) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, ms) <= 0)
            break;
        ssize_t r = read(fd, buf + got, cap - 1 - got);
        if (r <= 0)
            break;
        got += (size_t)r;
        buf[got] = '\0';
        if (want && strstr(buf, want))
            ms = 200;                   /* what we wanted is here: settle */
    }
    buf[got] = '\0';
    return got;
}
#endif

int main(void)
{
    int out = dup(1);
    rep = fdopen(out, "w");
    if (!rep)
        return 1;
    setvbuf(rep, NULL, _IOLBF, 0);
    fprintf(rep, "=== test_syssvc_crtl_devfd: the C RTL's terminals and pipes are VMS channels ===\n");
#if !defined(OVMX_CRTLFD_STATIC)
#error "test_syssvc_crtl_devfd is registered only where the C RTL file layer is linked (OVMX_CRTL_FD_STATIC, tests/qemu/CMakeLists.txt)"
#else
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL) || vms_kif_open() < 0) {
        fprintf(rep, "=== test_syssvc_crtl_devfd: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    /* the substrate's own terminal line underneath descriptors 0 and 1 */
    int dm = posix_openpt(O_RDWR | O_NOCTTY);
    int ds = -1;
    if (dm >= 0 && grantpt(dm) == 0 && unlockpt(dm) == 0)
        ds = open(ptsname(dm), O_RDWR | O_NOCTTY);
    check(ds >= 0 && dup2(ds, 0) == 0 && dup2(ds, 1) == 1,
          "descriptors 0 and 1 are a (decoy) terminal line of the substrate's");

    /* the VMS terminal SYS$INPUT / SYS$OUTPUT name */
    char dev[32] = "";
    int tm = -1;
    uint32_t vst = ovmx_vterm_create(dev, sizeof dev, &tm);
    check((vst & 1) && tm >= 0 && dev[0], "an RTAn: terminal unit of the executive's terminal driver");
    check(deflog("SYS$OUTPUT", dev) && deflog("SYS$INPUT", dev),
          "SYS$OUTPUT and SYS$INPUT name that terminal (LNM$PROCESS_TABLE)");

    uint32_t mounted = vms_kif_acp_mount(UNIT);
    ovmx_crtl_fd_install();

    /* ---- (a) stdout / stdin on a terminal: $QIO on SYS$'s channel ---- */
    {
        char nm[256] = "", seen[4096], decoy[4096];
        char want[32];
        snprintf(want, sizeof want, "%s", dev[0] == '_' ? dev + 1 : dev);
        if (want[0] && want[strlen(want) - 1] == ':')
            want[strlen(want) - 1] = '\0';
        char *gn = ovmx_crtl_getname(1, nm, 1);
        /* negctl: crtl-devfd-tty-via-kernel */
        check(gn && want[0] && strstr(nm, want) != NULL,
              "getname(1) is SYS$OUTPUT's terminal device");
        check(isatty(1) == 1, "isatty(1): the terminal driver answers for SYS$OUTPUT");
        printf("DEVFD-TT-OUT 4711\n");
        fflush(stdout);
        drain(tm, seen, sizeof seen, 3000, "DEVFD-TT-OUT 4711");
        drain(dm, decoy, sizeof decoy, 300, NULL);
        /* negctl: crtl-devfd-tty-via-kernel */
        check(strstr(seen, "DEVFD-TT-OUT 4711\r") != NULL,
              "printf() reached SYS$OUTPUT's terminal through the terminal driver (CR LF)");
        /* negctl: crtl-devfd-tty-via-kernel */
        check(strstr(decoy, "DEVFD-TT-OUT") == NULL,
              "...and nothing reached the substrate's line underneath descriptor 1");

        /* typed ahead on both lines: the RTAn: (SYS$INPUT) and the decoy
         * underneath descriptor 0, so a C RTL reading the wrong one returns
         * the wrong line instead of waiting */
        char line[128] = "";
        if (write(dm, "decoy line\r", 11) == 11 && write(tm, "hello vms\r", 10) == 10 &&
            fgets(line, sizeof line, stdin)) {
        }
        /* negctl: crtl-devfd-tty-via-kernel */
        check(strcmp(line, "hello vms\n") == 0,
              "fgets(stdin) read the line typed on SYS$INPUT's terminal (CR -> newline)");
    }

    /* ---- (b) pipe(): a mailbox pair ---- */
    {
        int p[2] = { -1, -1 };
        char mb[256] = "", b[64];
        uint32_t cls = 0, unit = 0;
        struct stat st;
        errno = 0;
        int pr = pipe(p);
        if (pr != 0)
            fprintf(rep, "  INFO: pipe() errno %d\n", errno);
        check(pr == 0, "pipe()");
        char *gn = ovmx_crtl_getname(p[0], mb, 1);
        /* negctl: crtl-devfd-pipe-via-kernel */
        check(gn && strncmp(mb + (mb[0] == '_'), "MBA", 3) == 0,
              "getname() of the read end is a mailbox (MBAn:)");
        /* negctl: crtl-devfd-pipe-via-kernel */
        check(mb[0] && (dvi(mb, DVI$_DEVCLASS, &cls) & 1) && cls == DC$_MAILBOX,
              "$GETDVI of that device: DC$_MAILBOX");
        /* negctl: crtl-devfd-pipe-via-kernel */
        check(mb[0] && (dvi(mb, DVI$_UNIT, &unit) & 1) && fstat(p[0], &st) == 0 &&
              S_ISFIFO(st.st_mode) && (uint32_t)st.st_ino == unit,
              "fstat() of the read end: a FIFO whose st_ino is the mailbox unit");
        /* both messages, then the end of file, queued before any read: a
         * reader can never wait here, whatever the descriptors are */
        ssize_t w1 = write(p[1], "abc", 3), w2 = write(p[1], "defg", 4);
        int wclosed = close(p[1]) == 0;
        ssize_t r1 = read(p[0], b, sizeof b);
        int ok1 = r1 == 3 && memcmp(b, "abc", 3) == 0;
        ssize_t r2 = read(p[0], b, sizeof b);
        int ok2 = r2 == 4 && memcmp(b, "defg", 4) == 0;
        /* negctl: crtl-devfd-pipe-via-kernel */
        check(w1 == 3 && w2 == 4 && ok1 && ok2, "each write() is one message; each read() takes one");
        check(wclosed && read(p[0], b, sizeof b) == 0,
              "close() of the write end: the reader sees end of file");
        check(close(p[0]) == 0, "close() of the read end");
        /* negctl: crtl-devfd-pipe-via-kernel */
        check(mb[0] && !(dvi(mb, DVI$_DEVCLASS, &cls) & 1),
              "with both ends closed the temporary mailbox is gone");
    }

    /* ---- (c) no C RTL name reaches the substrate's file system ---- */
    {
        int fd;
        struct stat st;
        errno = 0;
        fd = open("/proc/self/status", O_RDONLY);
        if (!(fd < 0 && errno == ENOENT))
            fprintf(rep, "  INFO: open(\"/proc/self/status\") = %d errno %d\n", fd, errno);
        /* negctl: crtl-kernel-namespace-passthru */
        check(fd < 0 && errno == ENOENT, "open(\"/proc/self/status\") fails ENOENT");
        if (fd >= 0) close(fd);
        errno = 0;
        /* negctl: crtl-kernel-namespace-passthru */
        check(stat("/sys/kernel", &st) != 0 && errno == ENOENT, "stat(\"/sys/kernel\") fails ENOENT");
        /* a substrate file that does exist under /run (made as the
         * substrate, inside its scope): the C RTL still cannot name it */
        ovmx_substrate_enter();
        (void)mkdir("/run", 0755);
        int probe = open("/run/devfdprobe", O_WRONLY | O_CREAT, 0644);
        if (probe >= 0) close(probe);
        ovmx_substrate_leave();
        errno = 0;
        /* negctl: crtl-kernel-namespace-passthru */
        check(probe >= 0 && access("/run/devfdprobe", F_OK) != 0 && errno == ENOENT,
              "access(\"/run/devfdprobe\") of a file the substrate made there fails ENOENT");
        errno = 0;
        fd = open("/dev/vms", O_RDWR);
        /* negctl: crtl-kernel-namespace-passthru */
        check(fd < 0 && errno == ENOENT, "open(\"/dev/vms\") fails ENOENT (the executive's door is not a C RTL file)");
        if (fd >= 0) close(fd);
        fd = open("/dev/null", O_WRONLY);
        check(fd >= 0 && write(fd, "x", 1) == 1, "/dev/null is the null device");
        if (fd >= 0) close(fd);

        int p[2];
        errno = 0;
        /* negctl-knockon: crtl-devfd-pipe-via-kernel */
        check(pipe(p) == 0 && openat(p[0], "x.c", O_RDONLY) < 0 && errno == ENOTDIR,
              "openat() relative to a pipe: ENOTDIR");
        close(p[0]); close(p[1]);
        errno = 0;
        check(openat(900, "x.c", O_RDONLY) < 0 && errno == EBADF,
              "openat() relative to no descriptor: EBADF");

        /* relative to an RMS directory descriptor: that directory */
        FILE *f = fopen(UNIT "[OVMXDIR]DEVFDAT.TXT", "w");
        int made = f && fputs("in ovmxdir\n", f) >= 0;
        if (f) made = fclose(f) == 0 && made;
        int d = open(UNIT "[OVMXDIR]", O_RDONLY | O_DIRECTORY);
        int g = d >= 0 ? openat(d, "devfdat.txt", O_RDONLY) : -1;
        char b[32] = "";
        ssize_t n = g >= 0 ? read(g, b, sizeof b - 1) : -1;
        check((mounted & 1) && made && d >= 0 && g >= 0 && n == 11 && memcmp(b, "in ovmxdir\n", 11) == 0,
              "openat() relative to an RMS directory descriptor opens the file in that directory");
        if (g >= 0) close(g);
        if (d >= 0) close(d);
        (void)unlink(UNIT "[OVMXDIR]DEVFDAT.TXT");

        ovmx_substrate_enter();
        fd = open("/proc/self/status", O_RDONLY);
        check(fd >= 0, "inside the substrate scope (a system service's own work) /proc is the kernel's");
        if (fd >= 0) close(fd);
        ovmx_substrate_leave();
    }

    if (mounted & 1)
        (void)vms_kif_acp_dmount(UNIT);
    if (tm >= 0)
        (void)ovmx_vterm_delete(dev, tm);
    fprintf(rep, "=== test_syssvc_crtl_devfd: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
#endif
}
