/*
 * init_aarch64.c - OVMX freestanding aarch64 proof, run as PID 1 (init)
 *                  under a real arm64 Linux kernel booted by qemu-system-aarch64.
 *
 * rd vms-f66 - aarch64 positioning win. This is the aarch64 analogue of the
 * Alpha syscall/crt0 proof (commit c0a663dc), but hoisted one rung higher:
 * the Alpha proof runs the freestanding layer under *user-mode* qemu-alpha
 * (syscalls translated to the host kernel), and every existing aarch64 proof
 * (the ~25 arm64 CI jobs) likewise runs under *user-mode* QEMU binfmt. This
 * program instead runs as the init process of a *real, system-emulated* arm64
 * Linux kernel (qemu-system-aarch64 -machine virt) -- so the OVMX freestanding
 * crt0/syscall layer is exercised against a genuine arm64 kernel at EL1/EL0,
 * not a syscall-translation shim on an x86_64 host.
 *
 * It links the SAME real OVMX freestanding sources the product uses
 * (arch/aarch64/{crt0,syscall,sigreturn}.S + vms_*.c): crt0.S provides _start,
 * calls __vms_runtime_init, then this main().
 *
 * Deterministic, PID-1-safe sequence (no filesystem, no /proc dependency):
 *   1. banner over the arm64 PL011 UART (fd 1 == console=ttyAMA0)
 *   2. getpid() -- we are init, must be 1
 *   3. uname(2) -- the machine field MUST read "aarch64" (in-guest positive
 *      architecture control: a binary that secretly ran on x86_64 would print
 *      "x86_64" here and fail the assertion below)
 *   4. read(-1) on a bad fd -- MUST return -EBADF (-9), the same negative-errno
 *      convention the whole C layer relies on (mirrors the Alpha proof)
 *   5. emit the PASS marker only if every check held, then exit_group(0)
 *
 * The run harness (../run_tests.sh) greps stdout for OVMX_AARCH64_PASS_MARKER.
 * Its negative control feeds an x86_64 build of this same program as /init to
 * the same arm64 boot: the arm64 kernel rejects the wrong-e_machine ELF, no
 * marker is ever printed, and the harness fails if it appears.
 */

#include "vms_syscall.h"
#include "vms_string.h"

#define PASS_MARKER "OVMX_AARCH64_SYSMODE_PASS"
#define FAIL_MARKER "OVMX_AARCH64_SYSMODE_FAIL"

/* reboot(2) magic (uapi/linux/reboot.h) -- power the guest off cleanly after
 * the verdict so the kernel does not panic on init returning. __NR_reboot is
 * not in vms_syscall.h's table, so name it per-arch here. */
#if defined(__aarch64__)
#define VMS_NR_REBOOT 142
#elif defined(__x86_64__)
#define VMS_NR_REBOOT 169
#else
#define VMS_NR_REBOOT (-1)
#endif
#define VMS_REBOOT_MAGIC1  0xfee1deadUL
#define VMS_REBOOT_MAGIC2  672274793UL       /* 0x28121969 */
#define VMS_REBOOT_POWEROFF 0x4321fedcUL

static void put(const char *s)
{
    vms_sys_write(1, s, vms_strlen(s));
}

/*
 * struct utsname: 6 NUL-terminated fields, each __NEW_UTS_LEN+1 == 65 bytes,
 * laid out sysname/nodename/release/version/machine/domainname. We only read
 * the machine field (field index 4). Declaring a flat buffer avoids depending
 * on a libc <sys/utsname.h> in this freestanding TU.
 */
#define UTS_FIELD 65
#define UTS_MACHINE_OFF (UTS_FIELD * 4)

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    int failures = 0;

    put("=== OVMX aarch64 freestanding init (PID 1 under qemu-system-aarch64) ===\n");

    /* 1. getpid: as the kernel's init we are PID 1. */
    {
        vms_pid_t pid = vms_sys_getpid();
        if (pid == 1) {
            put("  OK: getpid == 1 (we are init)\n");
        } else {
            put("  FAIL: getpid != 1\n");
            failures++;
        }
    }

    /* 2. uname: the running MACHINE must be aarch64. This is the in-guest
     *    positive architecture control -- proves the code executes on a real
     *    arm64 kernel, not a silently-x86_64 one. */
    {
        char uts[UTS_FIELD * 6];
        long r = __vms_syscall1(__NR_uname, (long)uts);
        const char *machine = uts + UTS_MACHINE_OFF;
        if (r == 0 && vms_strcmp(machine, "aarch64") == 0) {
            put("  OK: uname machine == aarch64 (");
            put(machine);
            put(")\n");
        } else {
            put("  FAIL: uname machine != aarch64 (");
            put(r == 0 ? machine : "uname failed");
            put(")\n");
            failures++;
        }
    }

    /* 3. read(bad fd) -> -EBADF (-9). Same negative-errno convention the whole
     *    freestanding C layer assumes (mirrors the Alpha proof). */
    {
        char b[8];
        vms_ssize_t n = vms_sys_read(-1, b, sizeof(b));
        if (n == -9) {
            put("  OK: read(bad fd) == -EBADF (-9)\n");
        } else {
            put("  FAIL: read(bad fd) != -EBADF\n");
            failures++;
        }
    }

    /* 4. write already exercised by every put() above; assert its return once
     *    explicitly for symmetry with the Alpha proof. */
    {
        static const char probe[] = "  OK: write() returns byte count\n";
        vms_ssize_t n = vms_sys_write(1, probe, sizeof(probe) - 1);
        if (n != (vms_ssize_t)(sizeof(probe) - 1)) {
            put("  FAIL: write() short count\n");
            failures++;
        }
    }

    if (failures == 0) {
        put(PASS_MARKER "\n");
    } else {
        put(FAIL_MARKER "\n");
    }

    /* Power the guest off cleanly (PSCI on -machine virt) so the run leaves no
     * spurious kernel panic in the log. If reboot(2) is refused, fall back to
     * exit_group -- the kernel then panics on init exit, which is benign: the
     * PASS marker has already been emitted and the harness keys on it. */
    __vms_syscall4(VMS_NR_REBOOT, (long)VMS_REBOOT_MAGIC1, (long)VMS_REBOOT_MAGIC2,
                   (long)VMS_REBOOT_POWEROFF, 0);
    vms_sys_exit_group(failures == 0 ? 0 : 1);
    return failures; /* unreachable */
}
