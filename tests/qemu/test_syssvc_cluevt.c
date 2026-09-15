/*
 * test_syssvc_cluevt.c - $SETCLUEVT / $CLRCLUEVT (FC-P3.8, vms-733) driven
 * against a REAL executive through /dev/vms via the freestanding kif client
 * (src/libvmssys/vms_kif.c) -- the same footing as test_syssvc_cluster_negctl.c.
 *
 * WHAT THIS PROVES. The cluster-event services are a translation layer over
 * the executive's single-slot registration (src/libvms/syssvc/sys_cluevt.c ->
 * vms_kif_cluster_setcluevt -> VMS_IOCTL_CLUSTER_SETCLUEVT ->
 * vms_cnxman_cluevt_set). On the node the QEMU harness boots, VAXCLUSTER is 0,
 * so the connection manager never started (cl->cnxman == NULL). The executive
 * therefore REFUSES the registration with SS$_NOSUCHDEV -- and the service must
 * return that refusal, not fabricate a successful arm. A registration nothing
 * can honour is the exact facade INV-6 exists to kill: a $SETCLUEVT that
 * answered SS$_NORMAL here would promise an AST the executive will never queue.
 *
 * The OPPOSITE half (a real arm answering SS$_NORMAL on a node whose connection
 * manager IS started, and the AST actually queued on a membership change) is a
 * live multi-node /dev/vms proof and belongs to the cluster lab, not this
 * booted-standalone suite -- the same split test_syssvc_cluster_negctl.c makes
 * between NOTMEMBER and NOSUCHDEV.
 *
 * Honest SKIP (77) when /dev/vms is absent: the cluster-event facility is
 * wholly executive-resident, so with no executive there is nothing to drive and
 * nothing this suite can fabricate (the test_syssvc_* honest-skip-77 contract).
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "vms_kif.h"
#include "starlet.h"
#include "cluevtdef.h"

#define SS_NORMAL       1u
#define SS_NOSUCHDEV    2680u
#define EXIT_SKIP       77

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

/* A cluster-event AST would land here; it is only ever an opaque address to
 * the service, never called in this suite (the arm is refused). */
static void cluevt_ast(void *prm) { (void)prm; }

/*
 * On a VAXCLUSTER=0 booted node the connection manager is not started
 * (cl->cnxman == NULL), so the executive refuses every cluster-event
 * registration with SS$_NOSUCHDEV. The services must relay that refusal
 * faithfully -- never fabricate a successful arm for an AST that will never be
 * queued. The one negctl (setcluevt-registers-without-cnxman) flips that
 * executive guard to SS$_NORMAL; every assertion below then reddens, which is
 * the proof this suite has teeth.
 */
static void check_refused_without_cnxman(void)
{
    unsigned int handle[2] = { 0u, 0u };
    uint32_t st;

    st = sys$setcluevt(CLUEVT$C_ADD, cluevt_ast, 0x1234u, 0u, handle);
    /* negctl: setcluevt-registers-without-cnxman */
    CHECK(st == SS_NOSUCHDEV,
          "$SETCLUEVT(ADD) with no connection manager -> SS$_NOSUCHDEV "
          "(arm refused by the executive, not fabricated)");

    st = sys$setcluevt(CLUEVT$C_REMOVE, cluevt_ast, 0x1234u, 0u, handle);
    /* negctl-knockon: setcluevt-registers-without-cnxman */
    CHECK(st == SS_NOSUCHDEV,
          "$SETCLUEVT(REMOVE) with no connection manager -> SS$_NOSUCHDEV");

    st = sys$clrcluevt(handle, 0u, 0u);
    /* negctl-knockon: setcluevt-registers-without-cnxman */
    CHECK(st == SS_NOSUCHDEV,
          "$CLRCLUEVT(handle) with no connection manager -> SS$_NOSUCHDEV");

    st = sys$clrcluevt((unsigned int *)0, 0u, CLUEVT$C_REMOVE);
    /* negctl-knockon: setcluevt-registers-without-cnxman */
    CHECK(st == SS_NOSUCHDEV,
          "$CLRCLUEVT(event) with no connection manager -> SS$_NOSUCHDEV");
}

int main(void)
{
    int probe;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_cluevt ===\n");

    probe = open("/dev/vms", O_RDWR);
    if (probe < 0) {
        printf("=== test_syssvc_cluevt: 0 passed, 0 failed (SKIPPED: "
               "no /dev/vms -- the cluster-event facility is "
               "executive-resident) ===\n");
        return EXIT_SKIP;
    }

    check_refused_without_cnxman();

    close(probe);
    printf("=== test_syssvc_cluevt: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
