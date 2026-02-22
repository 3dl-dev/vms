/*
 * ovmx_init.c - OVMX Boot Orchestrator (STARTUP.EXE)
 *
 * Unified PID 1 / ENTRYPOINT for OVMX on both Docker and bare-metal (QEMU).
 * Handles the entire boot sequence: filesystem setup, kernel module loading,
 * VMS directory provisioning, SYSUAF user provisioning, daemon startup,
 * boot banner, and console login loop.
 *
 * No shell scripts, no busybox, no /etc/passwd — SYSUAF is the user database.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <errno.h>

#include "vms/pcb.h"
#include "ovmx_layout.h"

#define LNM_SOCKET_PATH  "/tmp/ovmx/lnm.sock"
#define SYSDISK_DEV      "/dev/vda"
#define INITRAMFS_BACKUP "/tmp/initramfs_vms"

/* Binary search paths — Docker puts them in /usr/local/bin, QEMU in SYSEXE */
static const char *bin_search_dirs[] = {
    VMS_SYSEXE, "/usr/local/bin", "/bin", "/sbin", NULL
};
static const char *lib_search_dirs[] = {
    VMS_SYSLIB, "/usr/local/lib", "/lib", NULL
};

static const char *vms_months[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

static volatile sig_atomic_t shutdown_requested = 0;
static int blkdev_mode = 0;

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

/* ------------------------------------------------------------------ */
/* Bare-metal bootstrap                                               */
/* ------------------------------------------------------------------ */

static void load_kernel_module(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return;
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        syscall(SYS_finit_module, fd, "", 0);
        close(fd);
    }
}

/*
 * Recursive copy of a directory tree (for vmsfs backing dir).
 * Handles regular files, directories, and symlinks.
 */
static void copy_recursive(const char *src, const char *dst)
{
    struct stat st;
    if (stat(src, &st) != 0)
        return;

    mkdir(dst, st.st_mode & 07777);

    DIR *d = opendir(src);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;

        char src_path[512], dst_path[512];
        snprintf(src_path, sizeof(src_path), "%s/%s", src, ent->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, ent->d_name);

        struct stat es;
        if (lstat(src_path, &es) != 0)
            continue;

        if (S_ISDIR(es.st_mode)) {
            copy_recursive(src_path, dst_path);
        } else if (S_ISLNK(es.st_mode)) {
            char link_target[512];
            ssize_t len = readlink(src_path, link_target, sizeof(link_target) - 1);
            if (len > 0) {
                link_target[len] = '\0';
                symlink(link_target, dst_path);
            }
        } else if (S_ISREG(es.st_mode)) {
            int sfd = open(src_path, O_RDONLY);
            if (sfd < 0) continue;
            int dfd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, es.st_mode & 07777);
            if (dfd < 0) { close(sfd); continue; }
            char buf[4096];
            ssize_t n;
            while ((n = read(sfd, buf, sizeof(buf))) > 0)
                write(dfd, buf, (size_t)n);
            close(dfd);
            close(sfd);
        }
    }
    closedir(d);
}

/*
 * Bare-metal bootstrap: mount filesystems, set hostname, load kernel
 * modules, mount vmsfs. Called when running as PID 1 on bare metal
 * or QEMU — not inside a Docker container.
 *
 * Two modes:
 *   1. Block-device mode: /dev/vda (virtio disk) present — mount vmsfs
 *      directly on the block device. If blank, run INITIALIZE.EXE first.
 *   2. Overlay mode: no block device — copy /vms to backing dir, mount
 *      vmsfs overlay (ephemeral, existing behavior).
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

    /* Set hostname */
    sethostname("OVMX", 4);

    /* Load VMS kernel modules */
    load_kernel_module("/lib/modules/vms.ko");
    load_kernel_module("/lib/modules/vmsfs.ko");

    struct stat vms_st;
    if (stat(SYSDISK_MOUNT, &vms_st) != 0)
        return;  /* No system disk root in initramfs */

    /* Check for system disk (virtio block device) */
    struct stat vda_st;
    if (stat(SYSDISK_DEV, &vda_st) == 0 && S_ISBLK(vda_st.st_mode)) {
        printf("%%STARTUP-I-SYSDISK, mounting system disk DKA0:\n");

        /* Back up initramfs before mounting over it */
        copy_recursive(SYSDISK_MOUNT, INITRAMFS_BACKUP);

        /* Try mounting — succeeds if disk is already formatted */
        int rc = mount(SYSDISK_DEV, SYSDISK_MOUNT, "vmsfs", 0, NULL);
        if (rc != 0) {
            /* Disk is blank or unformatted — run INITIALIZE.EXE */
            printf("%%STARTUP-I-INIT, initializing blank system disk\n");

            char init_path[256];
            snprintf(init_path, sizeof(init_path),
                     "%s/SYS0/SYSCOMMON/SYSEXE/INITIALIZE.EXE",
                     INITRAMFS_BACKUP);

            pid_t pid = fork();
            if (pid == 0) {
                execl(init_path, "INITIALIZE.EXE",
                      SYSDISK_DEV, "OVMX", (char *)NULL);
                _exit(1);
            }
            if (pid > 0) {
                int ws;
                waitpid(pid, &ws, 0);
            }

            rc = mount(SYSDISK_DEV, SYSDISK_MOUNT, "vmsfs", 0, NULL);
        }

        if (rc == 0) {
            printf("%%STARTUP-I-MOUNTED, system disk DKA0: mounted\n");
            blkdev_mode = 1;
            return;
        }

        printf("%%STARTUP-W-MOUNTFAIL, system disk mount failed (errno=%d), using overlay\n",
               errno);
        /* Fall through to overlay mode */
    }

    /* Overlay mode — no system disk or block-device mount failed */
    mkdir("/var", 0755);
    mkdir("/var/vmsfs", 0755);

    /* Use backup if we already copied for a failed blkdev attempt */
    struct stat backup_st;
    if (stat(INITRAMFS_BACKUP, &backup_st) == 0)
        copy_recursive(INITRAMFS_BACKUP, "/var/vmsfs");
    else
        copy_recursive(SYSDISK_MOUNT, "/var/vmsfs");

    mount("none", SYSDISK_MOUNT, "vmsfs", 0, "backing=/var/vmsfs,case_blind=1");
}

/* ------------------------------------------------------------------ */
/* System install / boot separation                                   */
/* ------------------------------------------------------------------ */

/*
 * Check if the system is already installed on the system disk.
 * DCL.EXE in SYSEXE is the marker — if it exists (file or symlink),
 * a prior install populated the tree and we can skip straight to boot.
 */
static int is_system_installed(void)
{
    struct stat st;
    return (lstat(VMS_SYSEXE "/DCL.EXE", &st) == 0);
}

/*
 * Create the VMS directory tree following ODS-2 conventions.
 * mkdir is idempotent — safe to call even if directories already exist.
 *
 * MFD [000000]:         /vms/
 * [SYS0]                /vms/SYS0/
 * [SYS0.SYSCOMMON]      /vms/SYS0/SYSCOMMON/
 * SYS$SYSTEM [SYSEXE]   /vms/SYS0/SYSCOMMON/SYSEXE/
 * SYS$LIBRARY [SYSLIB]  /vms/SYS0/SYSCOMMON/SYSLIB/
 * SYS$MANAGER [SYSMGR]  /vms/SYS0/SYSCOMMON/SYSMGR/
 * SYS$HELP [SYSHLP]     /vms/SYS0/SYSCOMMON/SYSHLP/
 * [USERS]               /vms/USERS/
 */
static void provision_dirs(void)
{
    mkdir(SYSDISK_MOUNT, 0755);
    mkdir(SYSDISK_MOUNT "/SYS0", 0755);
    mkdir(SYSDISK_MOUNT "/SYS0/SYSCOMMON", 0755);
    mkdir(VMS_SYSEXE, 0755);
    mkdir(VMS_SYSLIB, 0755);
    mkdir(VMS_SYSMGR, 0755);
    mkdir(VMS_SYSHLP, 0755);
    mkdir(VMS_USERS, 0755);
    mkdir(SYSDISK_MOUNT "/SYSTMP", 0755);  /* SYS$SCRATCH */
    mkdir("/tmp/ovmx", 0755);
    mkdir("/tmp/ovmx/locks", 0755);
}

/*
 * Create a symlink at vms_path → discovered binary location.
 * Searches bin_search_dirs for the named file. Skips if vms_path
 * already exists (binary placed directly in SYS$SYSTEM by initramfs).
 */
static void ensure_vms_binary(const char *vms_path, const char *name)
{
    struct stat st;
    if (lstat(vms_path, &st) == 0)
        return;  /* Already exists */

    for (int i = 0; bin_search_dirs[i]; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", bin_search_dirs[i], name);
        if (stat(path, &st) == 0) {
            symlink(path, vms_path);
            return;
        }
    }
}

/*
 * Create a symlink at vms_path → discovered library location.
 */
static void ensure_vms_library(const char *vms_path, const char *name)
{
    struct stat st;
    if (lstat(vms_path, &st) == 0)
        return;

    for (int i = 0; lib_search_dirs[i]; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", lib_search_dirs[i], name);
        if (stat(path, &st) == 0) {
            symlink(path, vms_path);
            return;
        }
    }
}

/*
 * Populate SYSEXE and SYSLIB with VMS-named binaries/images.
 * On QEMU, files are already at VMS paths (initramfs). On Docker,
 * creates symlinks from SYSEXE/SYSLIB to /usr/local/{bin,lib}/.
 */
static void provision_symlinks(void)
{
    /* [SYS0.SYSCOMMON.SYSEXE] executables */
    ensure_vms_binary(VMS_SYSEXE "/LOGINOUT.EXE", "LOGINOUT.EXE");
    ensure_vms_binary(VMS_SYSEXE "/DCL.EXE", "DCL.EXE");
    ensure_vms_binary(VMS_SYSEXE "/HELP.EXE", "HELP.EXE");
    ensure_vms_binary(VMS_SYSEXE "/AUTHORIZE.EXE", "AUTHORIZE.EXE");
    ensure_vms_binary(VMS_SYSEXE "/MAIL.EXE", "MAIL.EXE");
    ensure_vms_binary(VMS_SYSEXE "/MONITOR.EXE", "MONITOR.EXE");
    ensure_vms_binary(VMS_SYSEXE "/VMSSSHD.EXE", "VMSSSHD.EXE");
    ensure_vms_binary(VMS_SYSEXE "/VMSLNMD.EXE", "VMSLNMD.EXE");
    ensure_vms_binary(VMS_SYSEXE "/STARTUP.EXE", "STARTUP.EXE");

    /* [SYS0.SYSCOMMON.SYSLIB] shareable images */
    ensure_vms_library(VMS_SYSLIB "/LIBVMS$SHR.EXE", "LIBVMS$SHR.EXE");
    ensure_vms_library(VMS_SYSLIB "/LIBVMSPROCESS$SHR.EXE", "LIBVMSPROCESS$SHR.EXE");
    ensure_vms_library(VMS_SYSLIB "/LIBVMSLNM$SHR.EXE", "LIBVMSLNM$SHR.EXE");
    ensure_vms_library(VMS_SYSLIB "/LIBVMSFS$SHR.EXE", "LIBVMSFS$SHR.EXE");
    ensure_vms_library(VMS_SYSLIB "/LIBVMSRMS$SHR.EXE", "LIBVMSRMS$SHR.EXE");
}

/*
 * Read SYSUAF and create home directories for each user.
 * SYSUAF format: USERNAME:HASH:UIC_GROUP:UIC_MEMBER:DEFAULT_DIR:FLAGS:PRIVS
 * No /etc/passwd — SYSUAF is the user database.
 */
static void provision_sysuaf_users(void)
{
    FILE *fp = fopen(VMS_SYSUAF_PATH, "r");
    if (!fp)
        return;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        /* Skip comments and blank lines */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0')
            continue;

        /* Extract default_dir (field 4, 0-indexed) */
        char *field = line;
        for (int i = 0; i < 4; i++) {
            field = strchr(field, ':');
            if (!field) break;
            field++;
        }
        if (!field)
            continue;

        /* Terminate at next colon or newline */
        char *end = field;
        while (*end && *end != ':' && *end != '\n' && *end != '\r')
            end++;
        *end = '\0';

        /* Create the home directory if non-empty */
        if (field[0] != '\0') {
            mkdir(field, 0755);
        }
    }
    fclose(fp);
}

/*
 * Install the OVMX system onto the system disk (overlay mode).
 * Creates ODS-2 directory tree, populates [SYS0.SYSCOMMON.SYSEXE]
 * and [SYS0.SYSCOMMON.SYSLIB] with symlinks to discovered binaries,
 * and provisions SYSUAF user home directories.
 */
static void install_system(void)
{
    printf("%%STARTUP-I-INSTALL, installing OVMX system\n");
    provision_dirs();
    provision_symlinks();
    provision_sysuaf_users();
    printf("%%STARTUP-I-INSTALLED, system installation complete\n");
}

/*
 * Install the OVMX system onto a block-device system disk.
 * Copies real binaries from the initramfs backup to the mounted
 * block device. No symlinks — vmsfs block-device mode stores real files.
 */
static void install_system_blkdev(void)
{
    printf("%%STARTUP-I-INSTALL, installing OVMX system to DKA0:\n");
    provision_dirs();
    copy_recursive(INITRAMFS_BACKUP, SYSDISK_MOUNT);
    provision_sysuaf_users();
    printf("%%STARTUP-I-INSTALLED, system installation complete\n");
}

/*
 * Try to start the logical name daemon.
 * Returns the child PID on success, -1 if vmslnmd not found.
 */
static pid_t start_lnm_daemon(void)
{
    struct stat st;
    if (stat(VMS_LNMD_PATH, &st) != 0) {
        return -1;  /* vmslnmd not available */
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: exec vmslnmd */
        execl(VMS_LNMD_PATH, "vmslnmd", (char *)NULL);
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
    if (stat(VMS_SSHD_PATH, &st) != 0) {
        return -1;  /* sshd not available */
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Detach from console terminal so vmssshd gets its own session */
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }
        /* Restore default signal handling */
        signal(SIGHUP, SIG_DFL);
        execl(VMS_SSHD_PATH, "vmssshd", (char *)NULL);
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
    if (stat(VMS_STARTUP_PATH, &st) != 0)
        return;

    pid_t pid = fork();
    if (pid == 0) {
        execl(VMS_DCL_PATH, "vmsdcl", VMS_STARTUP_PATH, (char *)NULL);
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

    /* Step 2: Install system if not already on the system disk */
    if (is_system_installed()) {
        printf("%%STARTUP-I-SYSBOOT, system disk detected, skipping install\n");
    } else {
        if (blkdev_mode)
            install_system_blkdev();
        else
            install_system();
    }

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
    int consecutive_failures = 0;

    while (!shutdown_requested) {
        /* Check if stdin is still open (avoid tight loop on EOF) */
        if (!isatty(STDIN_FILENO) && feof(stdin)) {
            break;
        }

        struct timespec t_before;
        clock_gettime(CLOCK_MONOTONIC, &t_before);

        pid_t child = fork();
        if (child == 0) {
            /* Child: exec vms_login */
            execl(VMS_LOGINOUT_PATH, "vms_login", (char *)NULL);
            /* If vms_login not found, exec vmsdcl directly */
            execl(VMS_DCL_PATH, "vmsdcl", (char *)NULL);
            /* Both failed — report why */
            fprintf(stderr, "%%STARTUP-F-NOLOGIN, cannot exec %s: %s\n",
                    VMS_LOGINOUT_PATH, strerror(errno));
            fprintf(stderr, "%%STARTUP-F-NOLOGIN, cannot exec %s: %s\n",
                    VMS_DCL_PATH, strerror(errno));
            _exit(1);
        } else if (child > 0) {
            /* Parent: wait for login session to end */
            int wstatus;
            waitpid(child, &wstatus, 0);

            struct timespec t_after;
            clock_gettime(CLOCK_MONOTONIC, &t_after);
            long elapsed_ms = (t_after.tv_sec - t_before.tv_sec) * 1000
                            + (t_after.tv_nsec - t_before.tv_nsec) / 1000000;

            /* Track consecutive fast failures (< 1 second) */
            if (elapsed_ms < 1000) {
                consecutive_failures++;
                if (consecutive_failures >= 5) {
                    fprintf(stderr,
                        "%%STARTUP-F-LOGINFAIL, login process failing repeatedly\n");
                    if (WIFEXITED(wstatus))
                        fprintf(stderr,
                            "%%STARTUP-F-LOGINFAIL, exit status %d\n",
                            WEXITSTATUS(wstatus));
                    else if (WIFSIGNALED(wstatus))
                        fprintf(stderr,
                            "%%STARTUP-F-LOGINFAIL, killed by signal %d\n",
                            WTERMSIG(wstatus));

                    /* Check if the binaries actually exist */
                    struct stat chk;
                    fprintf(stderr, "%%STARTUP-I-DIAG, %s: %s\n",
                            VMS_LOGINOUT_PATH,
                            stat(VMS_LOGINOUT_PATH, &chk) == 0 ?
                                "exists" : strerror(errno));
                    fprintf(stderr, "%%STARTUP-I-DIAG, %s: %s\n",
                            VMS_DCL_PATH,
                            stat(VMS_DCL_PATH, &chk) == 0 ?
                                "exists" : strerror(errno));

                    /* Back off instead of spinning */
                    sleep(5);
                    consecutive_failures = 0;
                }
            } else {
                consecutive_failures = 0;
            }

            if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0) {
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
