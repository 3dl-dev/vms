/*
 * ovmx_init.c - OVMX Boot Orchestrator
 *
 * This is the ENTRYPOINT for the OVMX Docker container.
 * It creates runtime directories, starts the logical name daemon,
 * displays a VMS-style boot banner, then enters a login loop
 * (fork+exec vms_login; on child exit, re-present login).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/syscall.h>

#include "vms/pcb.h"

#define VMS_LOGIN_PATH   "/vms/sys$system/LOGINOUT.EXE"
#define VMSDCL_PATH      "/vms/sys$system/DCL.EXE"
#define STARTUP_PATH     "/vms/sys$manager/STARTUP.COM"
#define VMSLNMD_PATH     "/vms/sys$system/VMSLNMD.EXE"
#define LNM_SOCKET_PATH  "/tmp/ovmx/lnm.sock"
#define SSHD_PATH        "/vms/sys$system/VMSSSHD.EXE"

static const char *vms_months[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

static volatile sig_atomic_t shutdown_requested = 0;

static void sigterm_handler(int sig)
{
    (void)sig;
    shutdown_requested = 1;
}

static void sigchld_handler(int sig)
{
    (void)sig;
    /* Reap all zombies */
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}

/*
 * Bare-metal bootstrap: mount essential filesystems and load kernel modules.
 * Called when ovmx_init is running as PID 1 on a real or virtual machine
 * (not inside a Docker container where these are already set up).
 */
static void bare_metal_init(void)
{
    /* Mount essential filesystems */
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount("tmpfs", "/tmp", "tmpfs", 0, NULL);
    mkdir("/dev/pts", 0755);
    mount("devpts", "/dev/pts", "devpts", 0, NULL);
    mkdir("/dev/shm", 0755);
    mount("tmpfs", "/dev/shm", "tmpfs", 0, NULL);

    /* Load kernel modules via finit_module */
    struct stat st;
    if (stat("/lib/modules/vms.ko", &st) == 0) {
        int fd = open("/lib/modules/vms.ko", O_RDONLY);
        if (fd >= 0) { syscall(SYS_finit_module, fd, "", 0); close(fd); }
    }
    if (stat("/lib/modules/vmsfs.ko", &st) == 0) {
        int fd = open("/lib/modules/vmsfs.ko", O_RDONLY);
        if (fd >= 0) { syscall(SYS_finit_module, fd, "", 0); close(fd); }
    }
}

/*
 * Create runtime directories needed by OVMX.
 */
static void create_runtime_dirs(void)
{
    mkdir("/tmp/ovmx", 0755);
    mkdir("/tmp/ovmx/locks", 0755);
    mkdir("/vms/sys$system", 0755);
    mkdir("/vms/sys$library", 0755);
    mkdir("/vms/sys$manager", 0755);
    mkdir("/vms/sys$login", 0755);
    mkdir("/vms/sys$help", 0755);
}

/*
 * Try to start the logical name daemon.
 * Returns the child PID on success, -1 if vmslnmd not found.
 */
static pid_t start_lnm_daemon(void)
{
    struct stat st;
    if (stat(VMSLNMD_PATH, &st) != 0) {
        return -1;  /* vmslnmd not available */
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: exec vmslnmd */
        execl(VMSLNMD_PATH, "vmslnmd", (char *)NULL);
        _exit(1);
    }
    return pid;
}

/*
 * Wait for the LNM daemon socket to appear (up to timeout_ms).
 */
static int wait_for_lnm_socket(int timeout_ms)
{
    struct stat st;
    int elapsed = 0;
    int interval = 50000; /* 50ms */

    while (elapsed < timeout_ms * 1000) {
        if (stat(LNM_SOCKET_PATH, &st) == 0) {
            return 1;  /* Socket appeared */
        }
        usleep((useconds_t)interval);
        elapsed += interval;
    }
    return 0;  /* Timed out */
}

/*
 * Try to start the SSH daemon for remote access.
 * Returns the child PID on success, -1 if sshd not found.
 */
static pid_t start_sshd(void)
{
    struct stat st;
    if (stat(SSHD_PATH, &st) != 0) {
        return -1;  /* sshd not available */
    }

    mkdir("/run/sshd", 0755);

    pid_t pid = fork();
    if (pid == 0) {
        /* Detach from console terminal so sshd gets its own session */
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }
        /* Restore default signal handling */
        signal(SIGHUP, SIG_DFL);
        execl(SSHD_PATH, SSHD_PATH, "-D", (char *)NULL);
        _exit(1);
    }
    return pid;
}

/*
 * Run SYS$MANAGER:STARTUP.COM via DCL subprocess.
 * Waits for completion before returning.
 */
static void run_startup(void)
{
    struct stat st;
    if (stat(STARTUP_PATH, &st) != 0)
        return;

    pid_t pid = fork();
    if (pid == 0) {
        execl(VMSDCL_PATH, "vmsdcl", STARTUP_PATH, (char *)NULL);
        _exit(1);
    }
    if (pid > 0) {
        int s;
        waitpid(pid, &s, 0);
    }
}

/*
 * Display VMS-style boot banner.
 */
static void display_boot_banner(int lnm_started, int sshd_started)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    printf("\n");
    printf("    OpenVMS V7.3\n");
    printf("    %2d-%s-%04d %02d:%02d:%02d.%02d\n\n",
           tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
           tm.tm_hour, tm.tm_min, tm.tm_sec,
           (int)(ts.tv_nsec / 10000000));

    printf("%%STDRV-I-STARTUP, OVMX startup begun at %2d-%s-%04d %02d:%02d:%02d.%02d\n",
           tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
           tm.tm_hour, tm.tm_min, tm.tm_sec,
           (int)(ts.tv_nsec / 10000000));

    if (lnm_started) {
        printf("%%STDRV-I-LNMSTART, logical name daemon started\n");
    }

    if (sshd_started) {
        printf("%%STDRV-I-SSHSTART, SSH remote access started on port 22\n");
    }

    /* Re-read time for "completed" message */
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);

    printf("%%STDRV-I-STARTUP, OVMX startup completed at %2d-%s-%04d %02d:%02d:%02d.%02d\n",
           tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
           tm.tm_hour, tm.tm_min, tm.tm_sec,
           (int)(ts.tv_nsec / 10000000));
    printf("\n");
}

/*
 * Main: boot sequence then login loop.
 */
int main(void)
{
    /* If we are PID 1 on bare metal, set up Linux plumbing */
    struct stat bm_st;
    if (getpid() == 1 && stat("/proc/version", &bm_st) != 0) {
        bare_metal_init();
    }

    /* Set up signal handling */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Reap zombies when running as PID 1 */
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa_chld.sa_mask);
    sigaction(SIGCHLD, &sa_chld, NULL);

    /* Ignore SIGHUP so login children can terminate without killing us */
    signal(SIGHUP, SIG_IGN);

    /* Step 1: Initialize SYSTEM process PCB with full privileges */
    struct vms_pcb *pcb = vms_pcb_init(0xFFFFFFFFFFFFFFFFULL);
    if (pcb) {
        uint32_t system_uic = (1 << 16) | 4;  /* [1,4] SYSTEM */
        vms_pcb_set_identity(1, system_uic, "SYSTEM", "SYSTEM");
        vms_pcb_set_default_dir("SYS$SYSROOT:[SYSMGR]");
    }

    /* Step 2: Create runtime directories */
    create_runtime_dirs();

    /* Step 3: Start logical name daemon */
    int lnm_started = 0;
    pid_t lnm_pid = start_lnm_daemon();
    if (lnm_pid > 0) {
        lnm_started = wait_for_lnm_socket(2000);
    }

    /* Step 4: Run STARTUP.COM */
    run_startup();

    /* Step 5: Start SSH daemon */
    int sshd_started = 0;
    pid_t sshd_pid = start_sshd();
    if (sshd_pid > 0) {
        sshd_started = 1;
    }

    /* Step 6: Boot banner */
    display_boot_banner(lnm_started, sshd_started);
    fflush(stdout);
    fflush(stderr);

    /* Step 7: Login loop */
    while (!shutdown_requested) {
        /* Check if stdin is still open (avoid tight loop on EOF) */
        if (!isatty(STDIN_FILENO) && feof(stdin)) {
            break;
        }

        pid_t child = fork();
        if (child == 0) {
            /* Child: exec vms_login */
            execl(VMS_LOGIN_PATH, "vms_login", (char *)NULL);
            /* If vms_login not found, exec vmsdcl directly */
            execl(VMSDCL_PATH, "vmsdcl", (char *)NULL);
            perror("exec");
            _exit(1);
        } else if (child > 0) {
            /* Parent: wait for login session to end */
            int wstatus;
            waitpid(child, &wstatus, 0);

            /* If child exited very quickly, stdin may be exhausted */
            if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0) {
                /* Brief pause to avoid tight loop if login keeps failing */
                usleep(100000);
            }

            /* Print blank line between sessions (like real VMS console) */
            printf("\n");
            fflush(stdout);
        } else {
            /* fork failed */
            perror("fork");
            sleep(1);
        }
    }

    /* Clean up: kill daemons if we started them */
    if (sshd_pid > 0) {
        kill(sshd_pid, SIGTERM);
        waitpid(sshd_pid, NULL, 0);
    }
    if (lnm_pid > 0) {
        kill(lnm_pid, SIGTERM);
        waitpid(lnm_pid, NULL, 0);
    }

    return 0;
}
