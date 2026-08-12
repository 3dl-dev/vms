/*
 * opcom_kmsg.c - /dev/kmsg -> operator surface bridge (rd vms-32a)
 *
 * See opcom_kmsg.h and docs/design-opcom-executive-logging.md for the full
 * design. Summary of what lives here:
 *
 *   opcom_kmsg_classify()            - pure suppress/route/destination +
 *                                       reformat decision, unit-tested
 *                                       directly (tests/ovmx_init/).
 *   opcom_kmsg_append_operator_log() - the OPERATOR.LOG writer for
 *                                       OPCOM_KMSG_OPERATOR_LOG records.
 *   opcom_kmsg_start()               - the /dev/kmsg reader thread that
 *                                       calls classify() against the real
 *                                       kernel log and dispatches each
 *                                       routed line to /dev/console or
 *                                       OPERATOR.LOG per its destination.
 */

/* _POSIX_C_SOURCE / _DEFAULT_SOURCE come from the ovmx_init target's own
 * compile definitions (CMakeLists.txt) -- not redefined here. */
#include "opcom_kmsg.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* SYS$MANAGER:OPERATOR.LOG path resolution -- the SAME accessor and the
 * SAME /tmp fallback src/libvms/syssvc/sys_operator.c's sys$sndopr uses
 * (see opcom_kmsg_append_operator_log() below), so both writers agree on
 * which physical file "OPERATOR.LOG" means at any point in boot. ovmx_init
 * already links vmsfs (CMakeLists.txt) for VMS-filespec translation
 * elsewhere in this executable; no new library dependency. */
#include "ovmx_layout.h"
#include "vmsfs/filespec.h"

/*
 * TWO FACILITIES (Rule 8 -- both OVMX design choices, neither VMS-authentic).
 *
 * OVMX -- vms.ko/vmsfs.ko's OWN records ("vms: "/"vmsfs: "-prefixed).
 * Loading a Linux kernel module and registering /dev/vms have no real-VMS
 * equivalent wording; VMS is never in this state, so no VMS facility name
 * is invented for it (Rule 10). "OVMX" is not a new invention for this
 * file -- it is the SAME facility src/ovmx_init/ovmx_init.c and
 * src/ovmx_provision/ovmx_provision.c already use for this exact class of
 * boot-orchestrator message ("%OVMX-I-EXEC, VMS executive attached on
 * /dev/vms", "%OVMX-F-EXECINIT, ..."). See docs/design-opcom-executive-
 * logging.md sec4 for the full accounting, including the per-ident table
 * below.
 *
 * SYSKRNL -- everything else the kernel says that clears the routing bar
 * (see opcom_kmsg_classify() below). Operator-ruling correction, 2026-08-12:
 * these lines are RE-STYLED, not suppressed -- they carry real host-health/
 * security information (a module-taint warning means an unverified
 * executive image loaded; a scheduling-latency warning is a real host
 * signal) that an operator would want, just wearing the wrong shape. A
 * DIFFERENT facility than OVMX keeps the two honest about their own origin:
 * "OVMX" means "OVMX's own executive/boot-orchestrator code said this",
 * "SYSKRNL" means "the Linux kernel layer underneath said this, re-styled."
 * The name reuses this repo's own Product and Kernel Layering vocabulary
 * (docs/architecture.md sec1, OVMX_SYSKRNL_NAME "OVMX/Linux") rather than
 * inventing a third term for the same idea.
 */
#define OPCOM_KMSG_FACILITY         "OVMX"
#define OPCOM_KMSG_SYSKRNL_FACILITY "SYSKRNL"

/*
 * opcom_kmsg_ident - which OVMX-invented ident (Rule 8) a routed vms:/
 * vmsfs: message wears. All vmsfs: records share one ident (the whole
 * module is "the filesystem"); vms: records get a more specific ident
 * where the text names a subsystem this file recognizes, and the module-
 * lifecycle catch-all otherwise. See the ident table in
 * docs/design-opcom-executive-logging.md sec4.
 */
static const char *opcom_kmsg_ident(const char *body)
{
    if (strstr(body, "disk unit") || strstr(body, "console terminal") ||
        strstr(body, "device table"))
        return "DEVTAB";
    if (strstr(body, "logical-name"))
        return "LNM";
    if (strstr(body, "mailbox"))
        return "MBX";
    if (strstr(body, "system identity"))
        return "SYSID";
    return "KMOD";
}

/*
 * The OPCOM-record banner (docs/design-opcom-executive-logging.md sec6) is
 * NOT built here -- that vocabulary belongs to sys$sndopr
 * (src/libvms/syssvc/sys_operator.c). Everything this file emits is a bare
 * "%FACILITY-S-IDENT, text" line, because vms.ko/vmsfs.ko's events (and the
 * re-styled SYSKRNL lines alongside them) are all pre-OPCOM boot-time
 * events (sec2) -- SYSKRNL lines landing in OPERATOR.LOG (below) is a
 * DESTINATION choice, not a vocabulary-B rewrite; they keep their bare
 * shape there too.
 *
 * CONSOLE-VS-LOG SPLIT (operator correction, 2026-08-12, round 2): PR #358
 * (vms-2213) deliberately keeps routine kernel printk off OPA0: so the boot
 * console matches the OpenVMS oracle (docs/design-boot-faithful.md). The
 * first cut of the SYSKRNL re-style (above) wrote every routed SYSKRNL line
 * straight to the console, which measurably reopened that leak: a CI
 * runner's real hardware/kernel emits far more NOTICE/WARNING-level
 * SYSKRNL chatter (X.509 cert loads, cpufreq driver probing, ...) than
 * this repo's minimal dev QEMU guest ever showed locally, flooding the
 * console and stalling the boot before Username: -- measured on PR #365.
 * The fix is a DESTINATION split, not a narrower filter: OVMX-facility
 * lines (genuinely vms.ko/vmsfs.ko's own, plus the vms:/vmsfs: prefix
 * collision, sec below) still go to the console -- that is what PR #358's
 * oracle-conformance baseline already expects and it has never been the
 * complaint. SYSKRNL-facility lines go to OPERATOR.LOG ONLY: the
 * information survives (an operator can TYPE/SEARCH it) but the LIVE boot
 * console stays exactly as VMS-shaped as #358 left it. The three
 * OPCOM_KMSG_* return values are defined in opcom_kmsg.h, alongside their
 * full contract.
 */
int opcom_kmsg_classify(int pri, const char *text, char *out, size_t outsz)
{
    int level;
    const char *body;
    const char *facility;
    const char *ident;
    char sev;
    int dest;

    if (!text || !out || outsz == 0)
        return OPCOM_KMSG_DROP;
    if (text[0] == '\0')
        return OPCOM_KMSG_DROP;

    level = pri & 7;

    /* Kernel debug records never reach the operator, even the routine
     * per-process trace vms-2213 already lowered to pr_debug for the
     * console -- this bridge must not reopen that leak through a second
     * path. Applies to BOTH facilities: OVMX's own debug chatter and
     * SYSKRNL's. */
    if (level >= 7)
        return OPCOM_KMSG_DROP;

    if (strncmp(text, "vms: ", 5) == 0) {
        body = text + 5;
        if (body[0] == '\0')
            return OPCOM_KMSG_DROP;
        facility = OPCOM_KMSG_FACILITY;
        ident = opcom_kmsg_ident(body);
        dest = OPCOM_KMSG_CONSOLE;
    } else if (strncmp(text, "vmsfs: ", 7) == 0) {
        body = text + 7;
        if (body[0] == '\0')
            return OPCOM_KMSG_DROP;
        facility = OPCOM_KMSG_FACILITY;
        ident = "VMSFS";
        dest = OPCOM_KMSG_CONSOLE;
    } else {
        /*
         * A SYSKRNL (Linux-kernel-layer) LINE: re-styled and routed to
         * OPERATOR.LOG by default (operator correction), except routine
         * INFO-level (and DEBUG-level, already excluded above) device/bus-
         * enumeration chatter -- ACPI, PCI, virtio, block-layer probe noise
         * -- which is the "genuinely zero operator value" bucket: real
         * VMS's own boot console does not surface internal driver-probe
         * minutiae either, and a fresh boot's kernel ring buffer holds
         * hundreds of such lines.
         *
         * THE THRESHOLD IS MEASURED, NOT GUESSED, against the two examples
         * the correction named explicitly: loading an unsigned out-of-tree
         * module makes the kernel itself print (measured against a real
         * boot, tests/qemu/test_job_control_console.sh):
         *
         *   vms: loading out-of-tree module taints kernel.            (level 4, WARNING)
         *   vms: module verification failed: ... - tainting kernel    (level 5, NOTICE)
         *
         * -- both below the cutoff. hrtimer's scheduling-latency warning
         * (kernel/time/hrtimer.c, printk_deferred(KERN_WARNING, ...)) is
         * level 4, same side of the line. So "route level<=5 (EMERG through
         * NOTICE), drop level 6 (INFO)" keeps both named examples and
         * excludes the routine-probe flood, without a per-string denylist
         * that would need to know every possible noise line's wording in
         * advance -- it also has no vms:/vmsfs: prefix, so there is nothing
         * mistaking it for OVMX's own record either.
         */
        if (level >= 6)
            return OPCOM_KMSG_DROP;
        body = text;
        facility = OPCOM_KMSG_SYSKRNL_FACILITY;
        ident = "KERNEL";
        dest = OPCOM_KMSG_OPERATOR_LOG;
    }

    /* A mechanical, honest translation of the severity the kernel already
     * computed -- not a judgment call this file is making. */
    switch (level) {
    case 0: case 1: case 2: sev = 'F'; break;   /* emerg/alert/crit */
    case 3:                 sev = 'E'; break;   /* err   (pr_err)   */
    case 4:                 sev = 'W'; break;   /* warn  (pr_warn)  */
    default:                sev = 'I'; break;   /* notice/info      */
    }

    snprintf(out, outsz, "%%%s-%c-%s, %s\n", facility, sev, ident, body);
    return dest;
}

/*
 * parse_kmsg_record - split one /dev/kmsg read() into its syslog priority
 * and message text.
 *
 * /dev/kmsg's record shape (documented, Documentation/ABI/testing/dev-kmsg):
 *   "<pri>,<seq>,<timestamp_us>,<flags>[,KEY=VALUE]*;<message text>\n"
 *   [optional "  KEY=VALUE\n" continuation/dictionary lines]
 *
 * Only <pri> and the message text (up to the first '\n' after ';') are
 * used; the sequence number, timestamp and any dictionary lines are not
 * needed here.
 */
static int parse_kmsg_record(const char *raw, size_t rawlen,
                              int *pri, char *msgbuf, size_t msgbufsz)
{
    const char *semi;
    const char *p, *nl;
    size_t mlen;

    if (!raw || rawlen == 0 || !pri || !msgbuf || msgbufsz == 0)
        return -1;

    semi = memchr(raw, ';', rawlen);
    if (!semi)
        return -1;

    *pri = (int)strtol(raw, NULL, 10);

    p = semi + 1;
    nl = memchr(p, '\n', (size_t)(raw + rawlen - p));
    mlen = nl ? (size_t)(nl - p) : (size_t)(raw + rawlen - p);
    if (mlen >= msgbufsz)
        mlen = msgbufsz - 1;
    memcpy(msgbuf, p, mlen);
    msgbuf[mlen] = '\0';
    return 0;
}

/*
 * SYS$MANAGER:OPERATOR.LOG fallback path, when the vmsfs translation
 * cannot resolve (no system disk mounted yet -- the SAME condition
 * sys_operator.c's open_operator_log() falls back for). Same literal path
 * as OPERATOR_LOG_FALLBACK in src/libvms/syssvc/sys_operator.c, so a
 * SYSKRNL line written before the system disk mounts and a sys$sndopr
 * record written in the same window land in the SAME file.
 */
#define OPCOM_KMSG_OPLOG_FALLBACK "/tmp/OPERATOR.LOG"

/*
 * opcom_kmsg_append_operator_log - append one already-formatted line to
 * OPERATOR.LOG. Resolves SYS$MANAGER:OPERATOR.LOG through vmsfs (the same
 * accessor sys$sndopr uses), falling back to /tmp when that path is not
 * yet writable (no system disk mounted -- OVMX's own established
 * convention for this exact condition, not a new invention here). Opens,
 * appends, and closes per call rather than holding a long-lived fd: the
 * resolvable path can CHANGE mid-boot (before vs. after the system disk
 * mounts), and re-resolving on every write is how a later call finds the
 * real file once it exists, without this thread needing to know when that
 * happens.
 */
static void opcom_kmsg_append_operator_log(const char *line)
{
    char oplog_linux[1024];
    FILE *f;

    if (vmsfs_to_linux_path(VMS_OPERATOR_LOG, oplog_linux, sizeof(oplog_linux)) == 1)
        f = fopen(oplog_linux, "a");
    else
        f = NULL;
    if (!f)
        f = fopen(OPCOM_KMSG_OPLOG_FALLBACK, "a");
    if (!f)
        return;  /* best-effort -- see opcom_kmsg_start()'s comment */

    fputs(line, f);
    fclose(f);
}

static void *opcom_kmsg_thread_main(void *arg)
{
    int kfd, cfd;
    char raw[8192];

    (void)arg;

    kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (kfd < 0)
        return NULL;  /* best-effort; see opcom_kmsg_start()'s comment */

    /* Replay every record still in the kernel ring buffer, including the
     * ones vms.ko/vmsfs.ko emitted before this thread started (module
     * load happens moments after this is called from bare_metal_init()). */
    lseek(kfd, 0, SEEK_SET);

    cfd = open("/dev/console", O_WRONLY | O_NOCTTY);
    if (cfd < 0)
        cfd = STDOUT_FILENO;

    for (;;) {
        struct pollfd pfd;
        int pr;
        ssize_t n;

        pfd.fd = kfd;
        pfd.events = POLLIN;
        pr = poll(&pfd, 1, 2000);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            continue;  /* periodic wake, nothing new -- keep following */

        n = read(kfd, raw, sizeof(raw) - 1);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            if (errno == EPIPE)
                continue;  /* this reader fell behind; some records were
                            * overwritten -- keep following from here */
            break;
        }
        if (n == 0)
            continue;
        raw[n] = '\0';

        int pri;
        char msg[512];
        char line[600];

        if (parse_kmsg_record(raw, (size_t)n, &pri, msg, sizeof(msg)) != 0)
            continue;

        int dest = opcom_kmsg_classify(pri, msg, line, sizeof(line));
        if (dest == OPCOM_KMSG_DROP)
            continue;

        if (dest == OPCOM_KMSG_OPERATOR_LOG) {
            /* SYSKRNL lines never reach the console (console-vs-log split,
             * PR #365 / #358): logged, not broadcast. */
            opcom_kmsg_append_operator_log(line);
            continue;
        }

        /* OPCOM_KMSG_CONSOLE -- OVMX's own records, unchanged from before
         * the console-vs-log split. */
        size_t len = strlen(line);
        size_t written = 0;
        while (written < len) {
            ssize_t w = write(cfd, line + written, len - written);
            if (w <= 0) {
                if (w < 0 && errno == EINTR)
                    continue;
                break;
            }
            written += (size_t)w;
        }
    }

    if (cfd != STDOUT_FILENO)
        close(cfd);
    close(kfd);
    return NULL;
}

void opcom_kmsg_start(void)
{
    pthread_t tid;
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    /* Best-effort: a bridge that fails to start must never block or fail
     * boot -- Rule 9's fail-honest gate is /dev/vms (executive_attach()),
     * not this console-visibility aid. */
    (void)pthread_create(&tid, &attr, opcom_kmsg_thread_main, NULL);
    pthread_attr_destroy(&attr);
}
