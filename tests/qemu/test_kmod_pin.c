/*
 * test_kmod_pin.c - The executive cannot be removed while OVMX is running
 *
 * WHAT THIS PROVES (epic vms-6b8 / item vms-0ff)
 *
 * The executive is INTEGRAL, not optional. OVMX's PID 1 refuses to bring the
 * system up unless /dev/vms opens, and then holds that descriptor open for
 * the life of the system (src/ovmx_init/ovmx_init.c, executive_attach). That
 * is what lets the system services drop their "what if the executive isn't
 * there?" paths entirely: the condition is unreachable by construction.
 *
 * The whole guarantee rests on one mechanical fact -- an open descriptor on
 * /dev/vms holds a reference on vms.ko, because vms_fops carries
 * .owner = THIS_MODULE (src/kernel/vms_module.c). If that ever stops being
 * true, mid-life loss of the executive becomes reachable again and every
 * deleted fallback becomes necessary again, silently. So the fact gets a
 * test rather than a comment.
 *
 * Tests:
 *   1. /sys/module/vms/refcnt rises when a descriptor is open.
 *   2. delete_module("vms") FAILS WITH EBUSY while a descriptor is open --
 *      i.e. `rmmod vms` cannot take the executive out from under a running
 *      system. This is the guarantee itself, so it is exercised for real
 *      rather than inferred from the refcount.
 *   3. The executive is still there afterwards (an ioctl still works), so a
 *      pass here means the module survived, not that the test got lucky.
 *
 * Deliberately NOT tested: unlinking the /dev/vms node. A privileged actor on
 * the host can still do that; it neither unloads nor disturbs the running
 * executive, but it would stop new processes opening it by path. OVMX does
 * not defend against a privileged actor sabotaging the running system, just
 * as VMS does not defend against one corrupting a resident executive image.
 * That boundary is stated in docs/design-executive-retrofit.md, not papered
 * over here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include "vms_ioctl.h"

#define SS_NORMAL 1

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

/* Read /sys/module/vms/refcnt. Returns -1 if unreadable. */
static long read_refcnt(void)
{
    FILE *f = fopen("/sys/module/vms/refcnt", "r");
    if (!f)
        return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1)
        v = -1;
    fclose(f);
    return v;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);  /* vms-b5b: line-buffer stdout so an unflushed fork() cannot splice output */
    printf("=== test_kmod_pin ===\n");

    long base = read_refcnt();
    CHECK(base >= 0, "/sys/module/vms/refcnt is readable (vms.ko is loaded)");
    if (base < 0)
        return 1;

    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0) {
        printf("  FAIL: cannot open /dev/vms\n");
        return 1;
    }

    /* 1. The open descriptor must hold a module reference. */
    long held = read_refcnt();
    /* negctl: executive-not-pinned */
    CHECK(held > base,
          "an open /dev/vms descriptor holds a reference on vms.ko");

    /*
     * 2. THE GUARANTEE: rmmod must fail while the executive is in use.
     *
     * O_NONBLOCK is what rmmod(8) itself passes: fail immediately rather than
     * waiting for the module to become idle. If this call ever SUCCEEDS, the
     * executive has just been unloaded out from under a running system --
     * exactly the event OVMX claims cannot happen -- so it is reported as the
     * failure it is, loudly.
     */
    errno = 0;
    long rc = syscall(SYS_delete_module, "vms", O_NONBLOCK);
    int err = errno;
    /* negctl: executive-not-pinned */
    CHECK(rc != 0,
          "rmmod vms is REFUSED while a descriptor is open (executive pinned)");

    /*
     * Assert the REASON, not just the refusal: a module that failed to unload
     * because it was never loaded, or because the name was wrong, would also
     * return nonzero and would prove nothing about pinning.
     *
     * EWOULDBLOCK (== EAGAIN) is the errno observed here, and it is the
     * correct one: under O_NONBLOCK the kernel refuses a still-referenced
     * module immediately rather than waiting for it to go idle, which is the
     * path rmmod(8) itself takes -- that is the "Module vms is in use" case.
     * EBUSY is accepted alongside it because the kernel uses EBUSY for a
     * module that is not in the LIVE state; either way the executive stayed
     * loaded because it was in use. Anything else means the unload was
     * refused for an unrelated reason and this test is not measuring the pin.
     */
    /* negctl: executive-not-pinned */
    CHECK(rc != 0 && (err == EWOULDBLOCK || err == EBUSY),
          "the refusal is specifically 'module is in use'");
    if (rc == 0) {
        printf("  NOTE: vms.ko was UNLOADED by this test. The executive is not"
               " pinned --\n        check that vms_fops sets .owner ="
               " THIS_MODULE in src/kernel/vms_module.c.\n");
    } else if (err != EWOULDBLOCK && err != EBUSY) {
        printf("  NOTE: delete_module failed with errno %d (%s), which is not"
               " an in-use refusal.\n", err, strerror(err));
    }

    /* 3. The executive must still be serving after the refused unload. */
    struct vms_register_args reg;
    memset(&reg, 0, sizeof(reg));
    reg.vms_pid = (uint32_t)getpid();
    /* NO init_privs (vms-2b8): VMS_IOCTL_REGISTER no longer carries a
     * privilege mask. A process naming its own privileges is the honor
     * system that item removed, in either direction. The executive now
     * DERIVES the mask from the task's real credentials. */
    int irc = ioctl(fd, VMS_IOCTL_REGISTER, &reg);
    CHECK(irc == 0 && reg.status == SS_NORMAL,
          "the executive still serves ioctls after the refused unload");

    close(fd);

    printf("=== test_kmod_pin: %d passed, %d failed ===\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
