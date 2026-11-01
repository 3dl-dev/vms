/*
 * test_opcom_kmsg.c - unit tests for opcom_kmsg_classify() (rd vms-32a).
 *
 * Compiles and links the REAL product translation unit
 * (src/ovmx_init/opcom_kmsg.c), not a copy -- so a drift in the shipped
 * route/re-style/format logic fails this test (CLAUDE.md Rule 10: a test
 * against a hand-maintained mock is worse than no test).
 *
 * opcom_kmsg_classify() is pure (no I/O, no /dev/kmsg, no kernel, no
 * writes to any shared file), so this runs anywhere ctest runs -- it does
 * not need a real kernel module or QEMU, and it cannot contend with other
 * concurrently-running tests or sessions over a shared OPERATOR.LOG (the
 * WRITE half, opcom_kmsg_append_operator_log(), is deliberately left to
 * the real-boot proof below rather than exercised directly here, precisely
 * to avoid that contention -- see tests/integration/test_opcom_record_
 * body.sh's own header for the shared-file hazard this sidesteps). The
 * end-to-end proof that BOTH facilities land in real OPERATOR.LOG and
 * NEITHER ever reaches the console is tests/qemu/test_job_control_
 * console.sh (extended by this item) and tests/qemu/
 * test_boot_conformance.sh (which pins the exact console sequence and
 * must see NOTHING this bridge adds), both of which boot the real image.
 *
 * THE CONSOLE IS NEVER A DESTINATION HERE (operator correction, round 3 --
 * the definitive fix). Two earlier cuts of this bridge routed OVMX-facility
 * lines, then also SYSKRNL lines, to /dev/console; both broke tests/qemu/
 * test_boot_conformance.sh's pinned oracle-derived console sequence, which
 * is produced entirely by the boot orchestrator and never by a kernel
 * module's printk. opcom_kmsg_classify() now has exactly two outcomes:
 * OPCOM_KMSG_DROP or OPCOM_KMSG_OPERATOR_LOG.
 *
 * WHAT EACH GROUP OF CASES PROVES, so this file stays falsifiable and not
 * just a change-detector:
 *
 *   1. genuine vms:/vmsfs: executive events ROUTE TO OPERATOR.LOG under the
 *      OVMX facility, reformatted as bare "%OVMX-<S>-<IDENT>, text" lines
 *      (docs/design-opcom-executive-logging.md sec3/sec4) -- NEVER the
 *      console.
 *   2. SYSKRNL (Linux-kernel-layer) lines RE-STYLE and ROUTE TO
 *      OPERATOR.LOG when their severity is NOTICE or more severe. Named
 *      after the exact two examples the operator ruling cited (module-
 *      taint, hrtimer), with their real MEASURED kernel severity levels (4
 *      and 5 respectively -- see the design doc sec3).
 *   3. routine INFO-level SYSKRNL chatter (no vms:/vmsfs: prefix, level
 *      6) is the one bucket still dropped entirely -- "genuinely zero
 *      operator value" per the ruling, not a re-opened suppression of the
 *      named examples.
 *   4. kernel debug-level records (level 7) are dropped for BOTH
 *      facilities -- the routine per-process trace line vms-2213 already
 *      moved to pr_debug for the console must not reopen through this
 *      second path, and routine kernel debug chatter is not operator-
 *      facing either.
 *   5. severity maps mechanically from the printk level the kernel already
 *      assigned (err->E, warn->W, notice/info->I).
 *   6. the measured vms:/vmsfs: prefix collision (Linux's OWN taint
 *      warning, printed as "%s: <text>" with the loading module's name
 *      substituted) is NOT special-cased: it is ordinary vms:-prefixed
 *      text and gets the OVMX/KMOD treatment (still OPERATOR.LOG, still
 *      never the console) like any other unrecognized vms: line --
 *      disclosed in the design doc sec4, not a defect.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "opcom_kmsg.h"
#include "opcom_exec_line.h"   /* rd vms-c56: shared with the NetBSD /dev/klog bridge */

static int failures = 0;

static const char *dest_name(int dest)
{
    switch (dest) {
    case OPCOM_KMSG_DROP:         return "DROP";
    case OPCOM_KMSG_OPERATOR_LOG: return "OPERATOR_LOG";
    default:                      return "?";
    }
}

static void check_dest(const char *label, int pri, const char *text,
                        int want_dest, const char *want_line)
{
    char out[600];
    int dest = opcom_kmsg_classify(pri, text, out, sizeof(out));

    if (dest != want_dest) {
        printf("  FAIL: %s -- expected %s, got %s\n", label,
               dest_name(want_dest), dest_name(dest));
        failures++;
        return;
    }
    if (want_dest != OPCOM_KMSG_DROP && strcmp(out, want_line) != 0) {
        printf("  FAIL: %s -- got %s want %s", label, out, want_line);
        failures++;
        return;
    }
    printf("  PASS: %s\n", label);
}

static void check_operator_log(const char *label, int pri, const char *text,
                                const char *want_line)
{
    check_dest(label, pri, text, OPCOM_KMSG_OPERATOR_LOG, want_line);
}

static void check_drop(const char *label, int pri, const char *text)
{
    check_dest(label, pri, text, OPCOM_KMSG_DROP, NULL);
}

/* --- seed-path capture (vms-98c2) ------------------------------------------
 * opcom_kmsg_seed_operator_log_from() replays the bridge's durable spool into an
 * `emit` callback. The callback has no context arg, so capture into a global. */
static char seed_cap[16][640];
static int  seed_n = 0;

static void seed_capture_emit(const char *line)
{
    if (seed_n < (int)(sizeof(seed_cap) / sizeof(seed_cap[0])))
        snprintf(seed_cap[seed_n], sizeof(seed_cap[0]), "%s", line);
    seed_n++;
}

/*
 * The genuine OPERATOR.LOG is seeded from the bridge's append-only spool, not
 * the fixed-size kernel ring buffer that could evict early boot records before
 * provision ran (the vms-98c2 flake). Prove the seed replays EVERY complete
 * spool record verbatim with the trailing LF stripped, skips a trailing partial
 * line (the reader thread caught mid-write), and emits nothing for a missing
 * spool -- all against a temp file, never the live spool.
 */
static void test_seed_from_spool(void)
{
    char path[] = "/tmp/opcom_seed_test_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        printf("  FAIL: seed test could not mkstemp\n");
        failures++;
        return;
    }
    const char *body =
        "%OVMX-I-SYSID, OpenVMX node OVMX\n"
        "%OVMX-I-LNM, logical names established\n"
        "%OVMX-I-MBX, mailbox subsystem ready\n"
        "%OVMX-I-VMSFS, mount in progr";   /* partial: no trailing '\n' */
    (void)write(fd, body, strlen(body));
    close(fd);

    seed_n = 0;
    opcom_kmsg_seed_operator_log_from(path, seed_capture_emit);
    unlink(path);

    if (seed_n != 3) {
        printf("  FAIL: seed replayed %d records, expected 3 "
               "(the trailing partial line must be skipped)\n", seed_n);
        failures++;
    } else if (strcmp(seed_cap[0], "%OVMX-I-SYSID, OpenVMX node OVMX") != 0 ||
               strcmp(seed_cap[1], "%OVMX-I-LNM, logical names established") != 0 ||
               strcmp(seed_cap[2], "%OVMX-I-MBX, mailbox subsystem ready") != 0) {
        printf("  FAIL: seed did not replay the spool records verbatim (LF-stripped)\n");
        failures++;
    } else {
        printf("  PASS: seed replays every complete spool record (LF-stripped), "
               "skips the partial (vms-98c2)\n");
    }

    seed_n = 0;
    opcom_kmsg_seed_operator_log_from("/tmp/opcom_seed_absent_zzzzzz", seed_capture_emit);
    if (seed_n != 0) {
        printf("  FAIL: seed emitted %d records from a nonexistent spool\n", seed_n);
        failures++;
    } else {
        printf("  PASS: seed from a missing spool emits nothing (best-effort)\n");
    }
}

/* rd vms-f47: the boot-message spool lives in /tmp, a tmpfs on NetBSD that
 * refuses writes below 4 MB of available memory. A spool append must say so. */
static void test_spool_refusal(void)
{
    char path[] = "/tmp/opcom_spool_testXXXXXX";
    int fd = mkstemp(path);
    FILE *rf;
    char got[64] = "";
    int full_fd = open("/dev/full", 1 /* O_WRONLY */);

    printf("\n--- spool append is checked (rd vms-f47) ---\n");
    /* NEGATIVE CONTROL: /dev/full opens fine and only the write fails, which is
     * exactly what the old open()+unchecked write() could not tell. */
    if (full_fd < 0) {
        printf("  FAIL: /dev/full is not available for the negative control\n");
        failures++;
    } else {
        ssize_t w = write(full_fd, "x", 1);
        if (w == 1) {
            printf("  FAIL: NEGCTL: a write to /dev/full succeeded\n");
            failures++;
        } else {
            printf("  PASS: NEGCTL: open() of the refusing spool succeeds, only write() fails\n");
        }
        close(full_fd);
    }
    if (fd < 0) {
        printf("  FAIL: no scratch dir\n");
        failures++;
        return;
    }
    close(fd);
    if (opcom_kmsg_spool_append(path, "%OVMX-I-KMOD, one\n") == 0 &&
        opcom_kmsg_spool_append(path, "%OVMX-I-KMOD, two\n") == 0) {
        rf = fopen(path, "r");
        if (rf) {
            size_t n = fread(got, 1, sizeof got - 1, rf);
            got[n] = '\0';
            fclose(rf);
        }
        if (strcmp(got, "%OVMX-I-KMOD, one\n%OVMX-I-KMOD, two\n") == 0)
            printf("  PASS: an accepted append lands whole, in order\n");
        else { printf("  FAIL: spool content '%s'\n", got); failures++; }
    } else {
        printf("  FAIL: append to a writable spool failed\n");
        failures++;
    }
    unlink(path);
    {
        int rc = opcom_kmsg_spool_append("/dev/full", "%OVMX-I-KMOD, lost\n");
        if (rc == -28 /* ENOSPC */)
            printf("  PASS: a refused spool append is REPORTED (-ENOSPC), not dropped silently\n");
        else { printf("  FAIL: refused append returned %d\n", rc); failures++; }
    }
}

int main(void)
{
    printf("=== test_opcom_kmsg: /dev/kmsg -> OPERATOR.LOG reformatter ===\n");

    /* --- 1. genuine executive events -> OPERATOR.LOG, under OVMX ------ */
    check_operator_log("vms: kernel-module init -> OPERATOR.LOG, KMOD, info",
                        6, "vms: initializing VMS kernel module",
                        "%OVMX-I-KMOD, initializing VMS kernel module\n");
    check_operator_log("vms: /dev/vms registered -> OPERATOR.LOG, KMOD, info",
                        6, "vms: /dev/vms registered successfully",
                        "%OVMX-I-KMOD, /dev/vms registered successfully\n");
    check_operator_log("vms: disk unit mapping -> OPERATOR.LOG, DEVTAB, info",
                        6, "vms: disk unit vda -> VDA0: (0:0)",
                        "%OVMX-I-DEVTAB, disk unit vda -> VDA0: (0:0)\n");
    check_operator_log("vms: console terminal created -> OPERATOR.LOG, DEVTAB, info",
                        6, "vms: device table initialized, console terminal OPA0: created",
                        "%OVMX-I-DEVTAB, device table initialized, console terminal OPA0: created\n");
    check_operator_log("vms: system identity constant -> OPERATOR.LOG, SYSID, info",
                        6, "vms: system identity constant SYSTEM [1,4] privileges=ALL established by the executive",
                        "%OVMX-I-SYSID, system identity constant SYSTEM [1,4] privileges=ALL established by the executive\n");
    check_operator_log("vms: logical-name arena -> OPERATOR.LOG, LNM, info",
                        6, "vms: logical-name arena ready (128 entries, 4096 bytes)",
                        "%OVMX-I-LNM, logical-name arena ready (128 entries, 4096 bytes)\n");
    check_operator_log("vms: mailbox table -> OPERATOR.LOG, MBX, info",
                        6, "vms: mailbox table initialized",
                        "%OVMX-I-MBX, mailbox table initialized\n");
    check_operator_log("vmsfs: mount -> OPERATOR.LOG, VMSFS, info",
                        6, "vmsfs: mounted block device, volume 'OVMXSYS', 40960 blocks",
                        "%OVMX-I-VMSFS, mounted block device, volume 'OVMXSYS', 40960 blocks\n");
    check_operator_log("vmsfs: registered -> OPERATOR.LOG, VMSFS, info",
                        6, "vmsfs: filesystem registered successfully",
                        "%OVMX-I-VMSFS, filesystem registered successfully\n");

    /* --- severity mapping, mechanical from the printk level ---------- */
    check_operator_log("vms: pr_err -> OPERATOR.LOG, E severity",
                        3, "vms: failed to register /dev/vms: -16",
                        "%OVMX-E-KMOD, failed to register /dev/vms: -16\n");
    check_operator_log("vms: pr_warn -> OPERATOR.LOG, W severity",
                        4, "vms: out of memory creating disk unit vdb (VDA100:)",
                        "%OVMX-W-DEVTAB, out of memory creating disk unit vdb (VDA100:)\n");
    check_operator_log("vmsfs: pr_err -> OPERATOR.LOG, E severity",
                        3, "vmsfs: unable to read home block",
                        "%OVMX-E-VMSFS, unable to read home block\n");

    /* --- 2. SYSKRNL lines RE-STYLE and ROUTE TO OPERATOR.LOG -----------
     * Operator-ruling correction, 2026-08-12: these two are the exact
     * examples named as carrying real operator-relevant information.
     * Severities are the REAL kernel levels, measured against an actual
     * boot (design doc sec3): hrtimer's scheduling-latency warning is
     * KERN_WARNING (level 4). */
    check_operator_log("hrtimer scheduling-latency warning -> OPERATOR.LOG, re-styled",
                        4, "hrtimer: interrupt took 123456 ns",
                        "%SYSKRNL-W-KERNEL, hrtimer: interrupt took 123456 ns\n");
    check_operator_log("an arbitrary WARNING-level kernel line also routes to OPERATOR.LOG",
                        4, "e1000: eth0 NIC Link is Up 1000 Mbps Full Duplex",
                        "%SYSKRNL-W-KERNEL, e1000: eth0 NIC Link is Up 1000 Mbps Full Duplex\n");
    check_operator_log("a NOTICE-level SYSKRNL line (the cutoff's inclusive edge) still routes",
                        5, "some-driver: entering compatibility mode",
                        "%SYSKRNL-I-KERNEL, some-driver: entering compatibility mode\n");
    check_operator_log("X.509 cert load (the exact CI-measured flood example) routes to OPERATOR.LOG",
                        5, "Loaded X.509 cert 'Canonical Ltd.: Secure Boot'",
                        "%SYSKRNL-I-KERNEL, Loaded X.509 cert 'Canonical Ltd.: Secure Boot'\n");
    check_operator_log("a bare substring match on 'vms' with no real prefix still gets SYSKRNL",
                        4, "systemd-vmsomething: unrelated warning line",
                        "%SYSKRNL-W-KERNEL, systemd-vmsomething: unrelated warning line\n");

    /* --- 3. routine INFO-level SYSKRNL chatter is the one dropped
     * bucket -- "genuinely zero operator value" (device/bus-enumeration
     * boilerplate), NOT a reopened suppression of the named examples. --- */
    check_drop("routine INFO-level SYSKRNL chatter is dropped (device-probe boilerplate)",
               6, "pci 0000:00:03.0: [1af4:1041] type 00 class 0x020000");
    check_drop("another routine INFO-level SYSKRNL line is dropped",
               6, "virtio_blk virtio2: [vda] 262144 512-byte logical blocks");

    /* --- 4. kernel debug level (7) is dropped for BOTH facilities ---- */
    check_drop("vms: debug-level record (routine per-process trace) stays off the operator",
               7, "vms: registered process pid=1 vms_pid=0x00000001 "
                  "uic=[1,4] job=0x00000001 privs=0x0 (SYSTEM)");
    check_drop("a debug-level SYSKRNL line stays off the operator too",
               7, "usb 1-1: new high-speed USB device number 2 using xhci_hcd");

    /* --- 6. the measured vms:/vmsfs: prefix collision is NOT special-
     * cased -- it is ordinary vms:-prefixed text, routed to OPERATOR.LOG
     * under OVMX/KMOD like any other unrecognized vms: line (design doc
     * sec4), and -- like every OVMX record -- never the console. -------- */
    check_operator_log("kernel's OWN taint line, with the 'vms: ' prefix collision, routes as OVMX/KMOD",
                        4, "vms: loading out-of-tree module taints kernel.",
                        "%OVMX-W-KMOD, loading out-of-tree module taints kernel.\n");
    check_operator_log("kernel's OWN signature-verification line, 'vms: ' collision, routes as OVMX/KMOD",
                        5, "vms: module verification failed: signature and/or "
                           "required key missing - tainting kernel",
                        "%OVMX-I-KMOD, module verification failed: signature and/or "
                        "required key missing - tainting kernel\n");

    /* --- edge cases ---------------------------------------------------- */
    check_drop("empty text is never routed", 6, "");
    check_drop("bare 'vms: ' with no body text is never routed", 6, "vms: ");
    check_drop("bare 'vmsfs: ' with no body text is never routed", 6, "vmsfs: ");

    /* --- 6b. rd vms-c56: the EXECUTIVE's own operator lines -- the
     *     diagnostics exec_console_printf() keeps off OPA0: at NOTICE, and the
     *     console lines it prints at ERR -- are filed in OPERATOR.LOG as they
     *     are: already VMS-form, the executive's, never re-styled SYSKRNL. --- */
    check_operator_log("executive diagnostic at NOTICE -> OPERATOR.LOG verbatim",
                        5, "%CNXMAN, no coordinator CSID seen yet: this node's CSID was NOT learned",
                        "%CNXMAN, no coordinator CSID seen yet: this node's CSID was NOT learned\n");
    check_operator_log("executive DLM diagnostic -> OPERATOR.LOG verbatim",
                        5, "%DLM, refusing a lock message (cat 0x02 op 0x01) from system 0x10001",
                        "%DLM, refusing a lock message (cat 0x02 op 0x01) from system 0x10001\n");
    check_operator_log("executive console line (kmsg-escaped leading newline) -> verbatim",
                        3, "\\x0a%CNXMAN,  now a VAXcluster member -- system OVMXA",
                        "%CNXMAN,  now a VAXcluster member -- system OVMXA\n");
    check_operator_log("a facility-severity-ident executive line -> verbatim",
                        5, "%CNXMAN-I-NODEPART, OVMX_CLEAN_DEPART is set",
                        "%CNXMAN-I-NODEPART, OVMX_CLEAN_DEPART is set\n");
    check_drop("an executive line at kernel DEBUG is still dropped", 7,
               "%CNXMAN, a debug trace");
    /* ... and nothing that merely starts with '%' is mistaken for one. */
    check_operator_log("a kernel line starting '%' but not a facility stays SYSKRNL",
                        4, "%5 of buffers used",
                        "%SYSKRNL-W-KERNEL, %5 of buffers used\n");
    /* The NetBSD /dev/klog form: the message buffer's uptime stamp first. */
    {
        const char *ex = opcom_exec_line("[    83.2700000] %PEA0, channel verified");
        if (ex && strcmp(ex, "%PEA0, channel verified") == 0) {
            printf("  PASS: NetBSD-stamped executive line recognised past its stamp\n");
        } else {
            printf("  FAIL: NetBSD-stamped executive line not recognised (%s)\n", ex ? ex : "(null)");
            failures++;
        }
        if (opcom_exec_line("[    83.2700000] mainbus0 (root)") == NULL) {
            printf("  PASS: a stamped NetBSD kernel line is not an executive line\n");
        } else {
            printf("  FAIL: a stamped NetBSD kernel line was taken for an executive line\n");
            failures++;
        }
    }

    /* --- 7. the seed path: spool -> genuine OPERATOR.LOG, lossless (vms-98c2) - */
    test_seed_from_spool();

    test_spool_refusal();

    printf("\n=== test_opcom_kmsg: %d failed ===\n", failures);
    return failures == 0 ? 0 : 1;
}
