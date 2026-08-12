/*
 * test_opcom_kmsg.c - unit tests for opcom_kmsg_classify() (rd vms-32a).
 *
 * Compiles and links the REAL product translation unit
 * (src/ovmx_init/opcom_kmsg.c), not a copy -- so a drift in the shipped
 * route/re-style/format logic fails this test (CLAUDE.md Rule 10: a test
 * against a hand-maintained mock is worse than no test).
 *
 * opcom_kmsg_classify() is pure (no I/O, no /dev/kmsg, no kernel), so this
 * runs anywhere ctest runs -- it does not need a real kernel module or
 * QEMU. The end-to-end proof that the real vms.ko/vmsfs.ko records, AND
 * real SYSKRNL (Linux-kernel-layer) lines, reach the real console in this
 * shape is tests/qemu/test_job_control_console.sh (extended by this
 * item), which boots the real image.
 *
 * WHAT EACH GROUP OF CASES PROVES, so this file stays falsifiable and not
 * just a change-detector:
 *
 *   1. genuine vms:/vmsfs: executive events ROUTE under the OVMX facility,
 *      reformatted as bare "%OVMX-<S>-<IDENT>, text" lines (docs/design-
 *      opcom-executive-logging.md sec3/sec4).
 *   2. SYSKRNL (Linux-kernel-layer) lines RE-STYLE and ROUTE under the SYSKRNL
 *      facility when their severity is NOTICE or more severe -- the
 *      operator-ruling correction (2026-08-12): these carry real
 *      diagnostic value and must NOT be dropped. Named after the exact two
 *      examples the ruling cited (module-taint, hrtimer), with their real
 *      MEASURED kernel severity levels (4 and 5 respectively -- see the
 *      design doc sec3).
 *   3. routine INFO-level SYSKRNL chatter (no vms:/vmsfs: prefix, level
 *      6) is the one bucket still dropped -- "genuinely zero operator
 *      value" per the ruling, not a re-opened suppression of the named
 *      examples.
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
 *      text and gets the OVMX/KMOD treatment like any other unrecognized
 *      vms: line -- disclosed in the design doc sec4, not a defect.
 */

#include <stdio.h>
#include <string.h>

#include "opcom_kmsg.h"

static int failures = 0;

static void check_route(const char *label, int pri, const char *text,
                         const char *want)
{
    char out[600];
    int routed = opcom_kmsg_classify(pri, text, out, sizeof(out));

    if (!routed) {
        printf("  FAIL: %s -- expected to route, was suppressed\n", label);
        failures++;
        return;
    }
    if (strcmp(out, want) != 0) {
        printf("  FAIL: %s -- got %s want %s", label, out, want);
        failures++;
        return;
    }
    printf("  PASS: %s\n", label);
}

static void check_suppress(const char *label, int pri, const char *text)
{
    char out[600];
    int routed = opcom_kmsg_classify(pri, text, out, sizeof(out));

    if (routed) {
        printf("  FAIL: %s -- expected to be suppressed, routed as: %s",
               label, out);
        failures++;
        return;
    }
    printf("  PASS: %s\n", label);
}

int main(void)
{
    printf("=== test_opcom_kmsg: /dev/kmsg -> operator-console reformatter ===\n");

    /* --- 1. genuine executive events route under OVMX, reformatted --- */
    check_route("vms: kernel-module init -> KMOD, info",
                6, "vms: initializing VMS kernel module",
                "%OVMX-I-KMOD, initializing VMS kernel module\n");
    check_route("vms: /dev/vms registered -> KMOD, info",
                6, "vms: /dev/vms registered successfully",
                "%OVMX-I-KMOD, /dev/vms registered successfully\n");
    check_route("vms: disk unit mapping -> DEVTAB, info",
                6, "vms: disk unit vda -> DKA0: (0:0)",
                "%OVMX-I-DEVTAB, disk unit vda -> DKA0: (0:0)\n");
    check_route("vms: console terminal created -> DEVTAB, info",
                6, "vms: device table initialized, console terminal OPA0: created",
                "%OVMX-I-DEVTAB, device table initialized, console terminal OPA0: created\n");
    check_route("vms: system identity constant -> SYSID, info",
                6, "vms: system identity constant SYSTEM [1,4] privileges=ALL established by the executive",
                "%OVMX-I-SYSID, system identity constant SYSTEM [1,4] privileges=ALL established by the executive\n");
    check_route("vms: logical-name arena -> LNM, info",
                6, "vms: logical-name arena ready (128 entries, 4096 bytes)",
                "%OVMX-I-LNM, logical-name arena ready (128 entries, 4096 bytes)\n");
    check_route("vms: mailbox table -> MBX, info",
                6, "vms: mailbox table initialized",
                "%OVMX-I-MBX, mailbox table initialized\n");
    check_route("vmsfs: mount -> VMSFS, info",
                6, "vmsfs: mounted block device, volume 'OVMXSYS', 40960 blocks",
                "%OVMX-I-VMSFS, mounted block device, volume 'OVMXSYS', 40960 blocks\n");
    check_route("vmsfs: registered -> VMSFS, info",
                6, "vmsfs: filesystem registered successfully",
                "%OVMX-I-VMSFS, filesystem registered successfully\n");

    /* --- severity mapping, mechanical from the printk level ---------- */
    check_route("vms: pr_err -> E severity",
                3, "vms: failed to register /dev/vms: -16",
                "%OVMX-E-KMOD, failed to register /dev/vms: -16\n");
    check_route("vms: pr_warn -> W severity",
                4, "vms: out of memory creating disk unit vdb (DKA100:)",
                "%OVMX-W-DEVTAB, out of memory creating disk unit vdb (DKA100:)\n");
    check_route("vmsfs: pr_err -> E severity",
                3, "vmsfs: unable to read home block",
                "%OVMX-E-VMSFS, unable to read home block\n");

    /* --- 2. SYSKRNL (Linux-kernel-layer) lines RE-STYLE and ROUTE, not suppressed --
     * Operator-ruling correction, 2026-08-12: these two are the exact
     * examples named as carrying real operator-relevant information. Their
     * severities are the REAL kernel levels, measured against an actual
     * boot (docs/design-opcom-executive-logging.md sec3): hrtimer's
     * scheduling-latency warning is KERN_WARNING (level 4). */
    check_route("hrtimer scheduling-latency warning -> SYSKRNL, re-styled and routed (not suppressed)",
                4, "hrtimer: interrupt took 123456 ns",
                "%SYSKRNL-W-KERNEL, hrtimer: interrupt took 123456 ns\n");
    check_route("an arbitrary WARNING-level kernel line also routes under SYSKRNL",
                4, "e1000: eth0 NIC Link is Up 1000 Mbps Full Duplex",
                "%SYSKRNL-W-KERNEL, e1000: eth0 NIC Link is Up 1000 Mbps Full Duplex\n");
    check_route("a NOTICE-level SYSKRNL line (the cutoff's inclusive edge) still routes",
                5, "some-driver: entering compatibility mode",
                "%SYSKRNL-I-KERNEL, some-driver: entering compatibility mode\n");
    check_route("a bare substring match on 'vms' with no real prefix still gets SYSKRNL, not OVMX",
                4, "systemd-vmsomething: unrelated warning line",
                "%SYSKRNL-W-KERNEL, systemd-vmsomething: unrelated warning line\n");

    /* --- 3. routine INFO-level SYSKRNL chatter is the one dropped
     * bucket -- "genuinely zero operator value" (device/bus-enumeration
     * boilerplate), NOT a reopened suppression of the named examples. --- */
    check_suppress("routine INFO-level SYSKRNL chatter is dropped (device-probe boilerplate)",
                   6, "pci 0000:00:03.0: [1af4:1041] type 00 class 0x020000");
    check_suppress("another routine INFO-level SYSKRNL line is dropped",
                   6, "virtio_blk virtio2: [vda] 262144 512-byte logical blocks");

    /* --- 4. kernel debug level (7) is dropped for BOTH facilities ---- */
    check_suppress("vms: debug-level record (routine per-process trace) stays off the operator",
                   7, "vms: registered process pid=1 vms_pid=0x00000001 "
                      "uic=[1,4] job=0x00000001 privs=0x0 (SYSTEM)");
    check_suppress("a debug-level SYSKRNL line stays off the operator too",
                   7, "usb 1-1: new high-speed USB device number 2 using xhci_hcd");

    /* --- 6. the measured vms:/vmsfs: prefix collision is NOT special-
     * cased -- it is ordinary vms:-prefixed text, routed under OVMX/KMOD
     * like any other unrecognized vms: line (design doc sec4). --------- */
    check_route("kernel's OWN taint line, with the 'vms: ' prefix collision, routes as OVMX/KMOD",
                4, "vms: loading out-of-tree module taints kernel.",
                "%OVMX-W-KMOD, loading out-of-tree module taints kernel.\n");
    check_route("kernel's OWN signature-verification line, 'vms: ' collision, routes as OVMX/KMOD",
                5, "vms: module verification failed: signature and/or "
                   "required key missing - tainting kernel",
                "%OVMX-I-KMOD, module verification failed: signature and/or "
                "required key missing - tainting kernel\n");

    /* --- edge cases ---------------------------------------------------- */
    check_suppress("empty text is never routed", 6, "");
    check_suppress("bare 'vms: ' with no body text is never routed", 6, "vms: ");
    check_suppress("bare 'vmsfs: ' with no body text is never routed", 6, "vmsfs: ");

    printf("\n=== test_opcom_kmsg: %d failed ===\n", failures);
    return failures == 0 ? 0 : 1;
}
