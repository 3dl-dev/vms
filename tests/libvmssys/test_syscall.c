/*
 * test_syscall.c - Test raw syscall wrappers
 *
 * Verifies: write, getpid, clock_gettime, getcwd, openat/read/close
 * Linked freestanding with custom _start via crt0.S
 */

#include "vms_syscall.h"
#include "vms_string.h"

static void write_str(const char *s)
{
    vms_sys_write(1, s, vms_strlen(s));
}

static void write_ok(const char *name)
{
    write_str("  OK: ");
    write_str(name);
    write_str("\n");
}

static void write_fail(const char *name)
{
    write_str("  FAIL: ");
    write_str(name);
    write_str("\n");
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    int failures = 0;

    write_str("=== libvmssys syscall test ===\n");

    /* Test 1: write */
    {
        vms_ssize_t n = vms_sys_write(1, "hello\n", 6);
        if (n == 6)
            write_ok("write");
        else {
            write_fail("write");
            failures++;
        }
    }

    /* Test 2: getpid */
    {
        vms_pid_t pid = vms_sys_getpid();
        if (pid > 0)
            write_ok("getpid");
        else {
            write_fail("getpid");
            failures++;
        }
    }

    /* Test 3: clock_gettime */
    {
        struct vms_timespec ts;
        int ret = vms_sys_clock_gettime(VMS_CLOCK_MONOTONIC, &ts);
        if (ret == 0 && ts.tv_sec >= 0)
            write_ok("clock_gettime");
        else {
            write_fail("clock_gettime");
            failures++;
        }
    }

    /* Test 4: getcwd */
    {
        char buf[256];
        vms_ssize_t ret = vms_sys_getcwd(buf, sizeof(buf));
        if (ret > 0 && buf[0] == '/')
            write_ok("getcwd");
        else {
            write_fail("getcwd");
            failures++;
        }
    }

    /* Test 5: openat/read/close on /proc/self/comm */
    {
        int fd = vms_sys_openat(VMS_AT_FDCWD, "/proc/self/comm", VMS_O_RDONLY, 0);
        if (fd >= 0) {
            char buf[64];
            vms_ssize_t n = vms_sys_read(fd, buf, sizeof(buf));
            vms_sys_close(fd);
            if (n > 0)
                write_ok("openat/read/close");
            else {
                write_fail("openat/read/close (read failed)");
                failures++;
            }
        } else {
            write_fail("openat (open failed)");
            failures++;
        }
    }

    /* Test 6: mmap/munmap */
    {
        void *p = vms_sys_mmap(NULL, 4096, VMS_PROT_READ | VMS_PROT_WRITE,
                               VMS_MAP_PRIVATE | VMS_MAP_ANONYMOUS, -1, 0);
        if (p != VMS_MAP_FAILED) {
            *(volatile char *)p = 42;
            int ret = vms_sys_munmap(p, 4096);
            if (ret == 0)
                write_ok("mmap/munmap");
            else {
                write_fail("munmap");
                failures++;
            }
        } else {
            write_fail("mmap");
            failures++;
        }
    }

    if (failures == 0)
        write_str("All syscall tests passed.\n");
    else
        write_str("Some syscall tests FAILED.\n");

    return failures;
}
