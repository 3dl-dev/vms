/*
 * test_kmod_resdir.c - DLM resource-directory + local-mastering scaffolding
 *                      (vms-ci.5 DB), driven through /dev/vms with raw ioctls.
 *
 * Proves the LOCAL parts of the distributed lock manager against a real
 * executive: an $ENQ resource is directory-hashed (to self, in a cluster of
 * one), locally mastered (self), and granted through the existing single-node
 * lock manager; the resource-directory + master structures REFLECT that; and a
 * second $ENQ on the same resource finds the already-assigned master rather
 * than re-mastering or re-creating the resource. The DLM state is read back
 * with VMS_IOCTL_GET_RESMASTER (a read-only diagnostic that does NOT create or
 * master a resource) -- nothing here hand-sets a structure or fakes a remote
 * master.
 *
 * REMOTE forwarding (a directory/master on another node) and remastering are
 * 0.4 (vms-ci.5 DC/DD) and are NOT exercised here: in a cluster of one the
 * directory hash always resolves to the local CSID, so that path is honestly
 * unreachable rather than faked.
 *
 * Grounding: IDSM lock-management "directory lookups" (mined transcript
 * ch6-part02, pp. 6-18..6-35); docs/design-cluster-node.md §5.
 *
 * Like the other test_kmod_* suites this returns nonzero when /dev/vms is
 * absent (the kernel-executive negative-control job requires it).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "vms_ioctl.h"

#define SS_NORMAL 1

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

static uint32_t resmaster(int fd, const char *name,
                          struct vms_resmaster_args *out)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->resnam, name, sizeof(out->resnam) - 1);
    ioctl(fd, VMS_IOCTL_GET_RESMASTER, out);
    return out->status;
}

static uint32_t enq(int fd, const char *name, uint32_t mode, uint32_t *lkid_out)
{
    struct vms_enq_args e;
    memset(&e, 0, sizeof(e));
    e.lkmode = mode;
    strncpy(e.resnam, name, sizeof(e.resnam) - 1);
    ioctl(fd, VMS_IOCTL_ENQ, &e);
    if (lkid_out)
        *lkid_out = e.lkid;
    return e.status;
}

static void deq(int fd, uint32_t lkid)
{
    struct vms_deq_args d;
    memset(&d, 0, sizeof(d));
    d.lkid = lkid;
    ioctl(fd, VMS_IOCTL_DEQ, &d);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_kmod_resdir ===\n");

    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0) {
        printf("  FAIL: cannot open /dev/vms\n");
        return 1;
    }

    struct vms_register_args reg = {0};
    reg.vms_pid = (uint32_t)getpid();
    ioctl(fd, VMS_IOCTL_REGISTER, &reg);
    CHECK(reg.status == SS_NORMAL, "register");

    struct vms_resmaster_args rm;

    /* ---- 1. A never-enqueued resource: directoried, but not mastered ---- */
    uint32_t st = resmaster(fd, "DLMRES1", &rm);
    CHECK(st == SS_NORMAL, "GET_RESMASTER on unmastered resource");
    CHECK(rm.local_csid != 0, "this node has a non-zero CSID");
    /* The directory node is a property of the NAME + membership, resolvable
     * before the resource exists. In a cluster of one it is the local node. */
    CHECK(rm.dir_csid == rm.local_csid, "directory hashes to self (cluster of one)");
    CHECK(rm.found == 0, "resource not yet present");
    CHECK(rm.master_csid == 0, "unmastered resource has master_csid 0");
    CHECK(rm.is_local_master == 0, "unmastered resource is not locally mastered");

    /* The diagnostic must NOT have created the resource. */
    resmaster(fd, "DLMRES1", &rm);
    CHECK(rm.found == 0, "GET_RESMASTER did not create the resource (read-only)");

    /* ---- 2. $ENQ masters it locally and grants through the lock mgr ---- */
    uint32_t lkid1 = 0;
    st = enq(fd, "DLMRES1", LCK_K_EXMODE, &lkid1);
    CHECK(st == SS_NORMAL, "ENQ EX on DLMRES1");
    CHECK(lkid1 != 0, "got a non-zero lock ID (granted locally)");

    st = resmaster(fd, "DLMRES1", &rm);
    CHECK(st == SS_NORMAL, "GET_RESMASTER after ENQ");
    CHECK(rm.found == 1, "resource now present");
    CHECK(rm.dir_csid == rm.local_csid, "directory is still self");
    CHECK(rm.master_csid == rm.local_csid, "resource is mastered on this node");
    CHECK(rm.is_local_master == 1, "is_local_master set");
    CHECK(rm.n_granted == 1, "one lock granted on the resource");

    /* ---- 3. A second $ENQ finds the EXISTING master (no re-master) ---- */
    uint32_t master_before = rm.master_csid;
    uint32_t lkid2 = 0, lkid3 = 0;
    st = enq(fd, "DLMRES2", LCK_K_CRMODE, &lkid2);
    CHECK(st == SS_NORMAL, "ENQ CR on DLMRES2 (first)");
    resmaster(fd, "DLMRES2", &rm);
    CHECK(rm.master_csid == rm.local_csid && rm.n_granted == 1,
          "DLMRES2 mastered locally, 1 granted");
    uint32_t res2_master = rm.master_csid;

    st = enq(fd, "DLMRES2", LCK_K_CRMODE, &lkid3);
    CHECK(st == SS_NORMAL, "ENQ CR on DLMRES2 (second, compatible)");
    resmaster(fd, "DLMRES2", &rm);
    CHECK(rm.found == 1, "DLMRES2 still the same resource block");
    CHECK(rm.master_csid == res2_master,
          "second ENQ found the existing master (master_csid unchanged)");
    CHECK(rm.n_granted == 2, "second lock attached to the mastered resource");

    /* DLMRES1's master must not have moved either. */
    resmaster(fd, "DLMRES1", &rm);
    CHECK(rm.master_csid == master_before, "DLMRES1 master stable across activity");

    /* Cleanup */
    deq(fd, lkid1);
    deq(fd, lkid2);
    deq(fd, lkid3);
    close(fd);

    printf("=== test_kmod_resdir: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
