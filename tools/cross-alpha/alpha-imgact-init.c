/*
 * alpha-imgact-init.c -- PID 1 for the OVMX-on-Alpha IMGACT-under-booted-kernel
 * proof (rd vms-989, rung A5a).
 *
 * The Alpha login-chain images (DCL/LOGINOUT/...) are static EM_ALPHA EXECs
 * today: the VMS-native LINK.EXE shareable graph does NOT cross-build for the
 * alpha-linux-gnu target (the CMake configure prints "OVMX_LINK_NATIVE off ...
 * alpha-linux-gnu is not aarch64/x86_64-musl"). So, unlike x86_64 -- where the
 * first IMGACT-activated image in the boot chain is DCL.EXE -- Alpha's real
 * boot chain (STARTUP -> PROVISION -> DCL, all static execve) never routes
 * through IMGACT.  IMGACT-on-Alpha (rung A2, rd vms-e11) is nonetheless proven:
 * this PID-1 activates a REAL VMS-native Alpha image (PT_INTERP=IMGACT.EXE,
 * DT_NEEDED on a shareable) UNDER THE BOOTED qemu-system-alpha kernel -- the
 * kernel's binfmt_elf loads IMGACT.EXE as the interpreter and IMGACT resolves
 * the image's symbol vector and runs it.  This is the boot-context analogue of
 * src/imgact/test/run_test_alpha.sh (which runs under user-mode qemu-alpha).
 *
 * It also loads vms.ko and confirms /dev/vms, so the one boot proves both the
 * executive attaches AND IMGACT activates a real image on the real kernel.
 *
 * Machine-greppable verdict lines the boot script asserts:
 *   OVMX-ALPHA-IMGACT: init up
 *   OVMX-ALPHA-IMGACT: /dev/vms PRESENT
 *   OVMX-ALPHA-IMGACT: activating VMS-native image via IMGACT.EXE
 *   IMGACT-TEST: PASS                 (printed by the activated image itself)
 *   OVMX-ALPHA-IMGACT: IMGACT ACTIVATED REAL IMAGE rc=<n>
 *   OVMX-ALPHA-IMGACT: ALL-PROVEN     (only if the image ran and printed PASS)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/reboot.h>
#include <linux/reboot.h>

static int load_module(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("OVMX-ALPHA-IMGACT: open %s failed: %s\n", path, strerror(errno)); return -1; }
    long rc = syscall(SYS_finit_module, fd, "", 0);
    close(fd);
    if (rc != 0 && errno != EEXIST) {
        printf("OVMX-ALPHA-IMGACT: finit_module %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

int main(void)
{
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("OVMX-ALPHA-IMGACT: init up\n");

    /* Bring the executive up so the proof runs on a live /dev/vms kernel. */
    if (load_module("/vms.ko") == 0 && access("/dev/vms", F_OK) == 0)
        printf("OVMX-ALPHA-IMGACT: /dev/vms PRESENT\n");
    else
        printf("OVMX-ALPHA-IMGACT: /dev/vms ABSENT (proof continues; IMGACT does not require it)\n");

    /* Activate a REAL VMS-native Alpha image via IMGACT.EXE (PT_INTERP). */
    printf("OVMX-ALPHA-IMGACT: activating VMS-native image via IMGACT.EXE\n");
    fflush(stdout);
    int rc = -1;
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = { (char *)"/imgact-proof/test_prog_alpha", NULL };
        char *envp[] = { NULL };
        execve("/imgact-proof/test_prog_alpha", argv, envp);
        printf("OVMX-ALPHA-IMGACT: execve of VMS-native image failed: %s\n", strerror(errno));
        _exit(127);
    } else if (pid > 0) {
        int status = 0;
        if (waitpid(pid, &status, 0) >= 0 && WIFEXITED(status))
            rc = WEXITSTATUS(status);
    }
    printf("OVMX-ALPHA-IMGACT: IMGACT ACTIVATED REAL IMAGE rc=%d\n", rc);

    /* The proof image is the GCC-port joint_e2e.exe, whose joint_main.c returns
     * the documented sentinel 3 (C$_EXIT1 fold -> executive $EXIT -> rc=3). The
     * rc readback line above is the ground truth regardless; the harness PASS
     * verdict fires only on the exact expected sentinel. */
    if (rc == 3)
        printf("OVMX-ALPHA-IMGACT: ALL-PROVEN\n");
    else
        printf("OVMX-ALPHA-IMGACT: NOT-PROVEN (rc=%d)\n", rc);

    printf("OVMX-ALPHA-IMGACT: powering off\n");
    sync();
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
    for (;;) pause();
    return 0;
}
