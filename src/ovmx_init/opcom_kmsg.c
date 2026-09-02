/*
 * opcom_kmsg.c - /dev/kmsg -> SYS$MANAGER:OPERATOR.LOG bridge (rd vms-32a)
 *
 * See opcom_kmsg.h and docs/design-opcom-executive-logging.md for the full
 * design. Summary of what lives here:
 *
 *   opcom_kmsg_classify()            - pure suppress/route + reformat
 *                                       decision, unit-tested directly
 *                                       (tests/ovmx_init/).
 *   opcom_kmsg_append_operator_log() - the OPERATOR.LOG writer for
 *                                       OPCOM_KMSG_OPERATOR_LOG records --
 *                                       the ONLY destination this file
 *                                       ever writes to. NOTHING here
 *                                       touches /dev/console; see
 *                                       opcom_kmsg.h's top comment for why.
 *   opcom_kmsg_start()               - the /dev/kmsg reader thread that
 *                                       calls classify() against the real
 *                                       kernel log and appends every
 *                                       routed line to OPERATOR.LOG.
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

/*
 * OPERATOR.LOG POST-FLIP (vms-aac, epic vms-208 atomic flip). The genuine
 * Files-11 SYS$MANAGER:OPERATOR.LOG lives on the ODS-2 ACP volume now, and it
 * is written the VMS way -- one RMS $PUT-at-EOF record per line -- which needs
 * RMS ($CREATE/$PUT over /dev/vms). This bridge runs in PID 1 (STARTUP.EXE),
 * which is statically linked and deliberately carries NO RMS/libvms: force-
 * binding the RMS record services into the static PID-1 image extracts the
 * whole vmsrms archive + ODS-2 codec and bloats the boot initramfs (the exact
 * overflow tests/qemu/rms_acp_bind.c documents). So PID 1 cannot itself write
 * the on-volume log.
 *
 * TWO honest destinations therefore split the job (Rule 9 / INV-6 -- no /vms
 * passthrough write anywhere):
 *   - the PID-1 follower thread appends routed lines to a private durable spool
 *     (opcom_kmsg_append_operator_log() below) -- a best-effort host-side copy
 *     and the pre-mount buffer, which NEVER masquerades as the on-volume log; and
 *   - opcom_kmsg_seed_operator_log() (below) lets an RMS-capable process
 *     (PROVISION.EXE, which runs after the SYS$DISK $MOUNT) replay that spool's
 *     boot records into the real OPERATOR.LOG over the ACP.
 */
#include "ovmx_layout.h"

/*
 * TWO FACILITIES (Rule 8 -- both OVMX design choices, neither VMS-authentic).
 * Both land in OPERATOR.LOG only (see opcom_kmsg.h) -- the facility split
 * is about ORIGIN, not destination.
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
 * "%FACILITY-S-IDENT, text" line, appended to OPERATOR.LOG. NEVER the
 * console -- see opcom_kmsg.h's top comment: tests/qemu/
 * test_boot_conformance.sh pins the exact console facility+ident sequence
 * against the OpenVMS oracle, and it is produced entirely by the boot
 * orchestrator, never by a kernel module's printk. Two earlier cuts of
 * this bridge (console for OVMX-facility lines; then also for SYSKRNL
 * lines) both broke that pinned sequence -- this is the definitive fix.
 */
int opcom_kmsg_classify(int pri, const char *text, char *out, size_t outsz)
{
    int level;
    const char *body;
    const char *facility;
    const char *ident;
    char sev;

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
    } else if (strncmp(text, "vmsfs: ", 7) == 0) {
        body = text + 7;
        if (body[0] == '\0')
            return OPCOM_KMSG_DROP;
        facility = OPCOM_KMSG_FACILITY;
        ident = "VMSFS";
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
    return OPCOM_KMSG_OPERATOR_LOG;
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
 * The PID-1 follower's local spool. This is NOT the system OPERATOR.LOG (that
 * lives on the ODS-2 ACP volume and is written by RMS-capable processes -- see
 * this file's top comment): it is a best-effort host-side copy of the routed
 * lines, on tmpfs, that PID 1 can always write without RMS. Deliberately a /tmp
 * path, NOT a /vms passthrough path -- the passthrough is retired (Rule 9).
 */
/*
 * OPCOM_KMSG_SEED_SPOOL - the PID-1 bridge's OWN durable, append-only capture of
 * every routed /dev/kmsg record, from boot. PRIVATE (0600, PID-1/root-owned) and
 * DISTINCT from the world-writable /tmp/OPERATOR.LOG that sys$sndopr uses as its
 * pre-mount fallback: PROVISION.EXE seeds the genuine on-volume OPERATOR.LOG from
 * THIS file (opcom_kmsg_seed_operator_log()), so it must not be a path another
 * process can plant lines into -- that would be a log-injection vector into the
 * executive log. (The kernel ring buffer, the old seed source, could not be
 * injected; a world-writable spool can -- vms-887.) A dotfile keeps it out of a
 * bare listing of /tmp; the 0600 create keeps it PID-1-private.
 */
#define OPCOM_KMSG_SEED_SPOOL "/tmp/.ovmx-opcom-seed.log"

/*
 * opcom_kmsg_append_operator_log - append one already-formatted routed line to
 * the bridge's private seed spool. Opens/appends/closes per call with a 0600
 * create (best-effort; a failure never blocks or fails boot -- see
 * opcom_kmsg_start()'s comment). This spool is the COMPLETE record of the
 * bridge's routed lines -- an append-only file never wraps, unlike the kernel
 * ring buffer -- and is what seeds the genuine on-volume OPERATOR.LOG at
 * provision (opcom_kmsg_seed_operator_log()); it does not, and must not, stand
 * in for that log to a reader (INV-6).
 */
static void opcom_kmsg_append_operator_log(const char *line)
{
    int fd = open(OPCOM_KMSG_SEED_SPOOL, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd < 0)
        return;  /* best-effort -- see opcom_kmsg_start()'s comment */

    (void)write(fd, line, strlen(line));
    close(fd);
}

static void *opcom_kmsg_thread_main(void *arg)
{
    int kfd;
    char raw[8192];

    (void)arg;

    kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (kfd < 0)
        return NULL;  /* best-effort; see opcom_kmsg_start()'s comment */

    /* Replay every record still in the kernel ring buffer, including the
     * ones vms.ko/vmsfs.ko emitted before this thread started (module
     * load happens moments after this is called from bare_metal_init()). */
    lseek(kfd, 0, SEEK_SET);

    /* NO /dev/console fd here -- this bridge never writes to the console
     * (opcom_kmsg.h's top comment). */

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

        if (opcom_kmsg_classify(pri, msg, line, sizeof(line)) == OPCOM_KMSG_DROP)
            continue;

        opcom_kmsg_append_operator_log(line);
    }

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
     * not this operator-visibility aid. */
    (void)pthread_create(&tid, &attr, opcom_kmsg_thread_main, NULL);
    pthread_attr_destroy(&attr);
}

/*
 * opcom_kmsg_seed_operator_log_from - replay every complete record in `path`
 * (the bridge's append-only seed spool, one classified line each) through
 * `emit`, with the trailing '\n' stripped -- one record's worth of text, ready
 * for RMS $PUT. A trailing partial line (the reader thread caught mid-write) has
 * no '\n' and is skipped. Split out with an explicit path so the seed logic is
 * unit-testable against a temp file without touching the live spool.
 *
 * Best-effort and side-effect-free on failure: if `path` cannot be opened it
 * emits nothing. `emit` must tolerate being called zero or many times.
 */
void opcom_kmsg_seed_operator_log_from(const char *path,
                                       void (*emit)(const char *line))
{
    FILE *f;
    char line[640];

    if (!emit || !path)
        return;

    f = fopen(path, "r");
    if (!f)
        return;

    while (fgets(line, sizeof(line), f)) {
        size_t llen = strlen(line);
        if (llen == 0 || line[llen - 1] != '\n')
            continue;   /* incomplete trailing record -- not fully written yet */
        line[llen - 1] = '\0';   /* RMS $PUT wants the record without the LF */
        emit(line);
    }

    fclose(f);
}

/*
 * opcom_kmsg_seed_operator_log - seed the genuine Files-11 OPERATOR.LOG from the
 * bridge's durable seed spool. This is the seam that puts the boot-time
 * vms.ko/vmsfs.ko events (and any re-styled SYSKRNL warning) into the on-volume
 * OPERATOR.LOG over the ACP: PID 1 cannot do that write itself (no RMS -- see
 * this file's top comment), so PROVISION.EXE -- which links RMS and runs AFTER
 * the SYS$DISK $MOUNT -- calls this with an `emit` that does
 * rms_textfile_append_line(SYS$MANAGER:OPERATOR.LOG, line).
 *
 * The source is the append-only seed spool, NOT the kernel ring buffer. The ring
 * is fixed-size and can EVICT the earliest boot records before provision runs --
 * that was the vms-98c2 flake: SYSID/LNM/MBX intermittently missing from
 * OPERATOR.LOG because the ring wrapped between boot and the once-only drain,
 * worse under a slow (TCG) boot. The spool never wraps, so every routed record
 * survives boot -> provision and the seed is deterministic. One-shot: it replays
 * what the spool holds and returns; it does not follow live.
 */
void opcom_kmsg_seed_operator_log(void (*emit)(const char *line))
{
    opcom_kmsg_seed_operator_log_from(OPCOM_KMSG_SEED_SPOOL, emit);
}
