/*
 * vms_rootaudit.c - ROOTAUDIT.EXE: list every process that runs as the
 * substrate's superuser (rd vms-251b, epic vms-8e6).
 *
 * OVMX's rule (Baron, 2026-10-10): nothing in OVMX runs as substrate root.
 * Root belongs to the substrate kernel; a VMS process gets its power only
 * from VMS privileges the executive checks. This utility measures that rule
 * on a running system, independently of the executive: it walks the
 * substrate's own process list and reports each process -- other than a
 * kernel thread -- any of whose threads has real, effective, saved or
 * file-system user id 0, or (Linux) holds any effective capability.
 *
 *   $ RUN SYS$SYSTEM:ROOTAUDIT
 *   %ROOTAUDIT-W-SUBSTRATEROOT, pid 1 STARTUP.EXE uid 0/0/0/0 capeff 000001ffffffffff
 *   %ROOTAUDIT-I-SUMMARY, 1 process(es) run as substrate root
 *
 * Exit: SS$_NORMAL when none, a warning (%X10000000 | ...) otherwise, so a
 * DCL procedure can test $STATUS. Linux: /proc/<pid>/status and stat (PF_KTHREAD
 * marks a kernel thread); NetBSD: sysctl kern.proc2 (P_SYSTEM marks one).
 * Reading either needs no privilege.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STS_NORMAL   1u
#define STS_ROOTSEEN 0x10000000u    /* inhibit-message | warning severity 0 */

static unsigned n_root;

static void report(long pid, const char *name, long r, long e, long s, long f,
                   unsigned long long cap)
{
    printf("%%ROOTAUDIT-W-SUBSTRATEROOT, pid %ld %s uid %ld/%ld/%ld/%ld capeff %016llx\n",
           pid, name, r, e, s, f, cap);
    n_root++;
}

#if defined(__NetBSD__)
#include <sys/param.h>
#include <sys/sysctl.h>

static int scan(void)
{
    int mib[6] = { CTL_KERN, KERN_PROC2, KERN_PROC_ALL, 0,
                   (int)sizeof(struct kinfo_proc2), 0 };
    size_t len = 0;
    if (sysctl(mib, 6, NULL, &len, NULL, 0) < 0)
        return -1;
    len += len / 4 + sizeof(struct kinfo_proc2) * 8;
    struct kinfo_proc2 *kp = malloc(len);
    if (!kp)
        return -1;
    mib[5] = (int)(len / sizeof(struct kinfo_proc2));
    if (sysctl(mib, 6, kp, &len, NULL, 0) < 0) {
        free(kp);
        return -1;
    }
    size_t n = len / sizeof(struct kinfo_proc2);
    for (size_t i = 0; i < n; i++) {
        if (kp[i].p_pid == 0 || (kp[i].p_flag & P_SYSTEM))
            continue;                    /* the kernel and its threads */
        if (kp[i].p_ruid == 0 || kp[i].p_uid == 0 || kp[i].p_svuid == 0)
            report((long)kp[i].p_pid, kp[i].p_comm, (long)kp[i].p_ruid,
                   (long)kp[i].p_uid, (long)kp[i].p_svuid, (long)kp[i].p_uid, 0);
    }
    free(kp);
    return 0;
}
#else
/* Linux. A kernel thread carries PF_KTHREAD in /proc/<pid>/stat field 9. */
#define PF_KTHREAD_FLAG 0x00200000ul

static int is_kthread(const char *pid)
{
    char path[300], buf[1024];
    snprintf(path, sizeof path, "/proc/%s/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return 1;                        /* gone: nothing to report */
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    char *p = strrchr(buf, ')');         /* comm may contain spaces */
    if (!p)
        return 1;
    unsigned long flags = 0;
    /* after ") ": state ppid pgrp session tty_nr tpgid flags */
    if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %lu", &flags) != 1)
        return 1;
    return (flags & PF_KTHREAD_FLAG) != 0;
}

/* One thread's status: uid fields and CapEff. 0 when readable. */
static int read_status(const char *path, char *name, size_t nsz, long u[4],
                       unsigned long long *cap)
{
    char line[256];
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f)) {
        if (name && !strncmp(line, "Name:", 5))
            sscanf(line + 5, " %63s", name);
        else if (!strncmp(line, "Uid:", 4))
            sscanf(line + 4, " %ld %ld %ld %ld", &u[0], &u[1], &u[2], &u[3]);
        else if (!strncmp(line, "CapEff:", 7))
            sscanf(line + 7, " %llx", cap);
    }
    fclose(f);
    (void)nsz;
    return 0;
}

static int is_root(const long u[4], unsigned long long cap)
{
    return u[0] == 0 || u[1] == 0 || u[2] == 0 || u[3] == 0 || cap != 0;
}

/* Linux credentials are per THREAD, so every thread of a process is read
 * (/proc/<pid>/task/<tid>/status): a process whose main thread dropped root
 * but which kept a root thread is reported. */
static int scan(void)
{
    DIR *d = opendir("/proc");
    struct dirent *de;
    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)de->d_name[0]) || is_kthread(de->d_name))
            continue;
        char path[300], name[64] = "?";
        long u[4] = { -1, -1, -1, -1 };
        unsigned long long cap = 0;
        snprintf(path, sizeof path, "/proc/%s/status", de->d_name);
        if (read_status(path, name, sizeof name, u, &cap) != 0)
            continue;
        int hit = is_root(u, cap);
        snprintf(path, sizeof path, "/proc/%s/task", de->d_name);
        DIR *td = hit ? NULL : opendir(path);
        struct dirent *te;
        while (td && (te = readdir(td)) != NULL) {
            if (!isdigit((unsigned char)te->d_name[0]))
                continue;
            long tu[4] = { -1, -1, -1, -1 };
            unsigned long long tcap = 0;
            char tp[600];
            snprintf(tp, sizeof tp, "/proc/%s/task/%s/status", de->d_name, te->d_name);
            if (read_status(tp, NULL, 0, tu, &tcap) == 0 && is_root(tu, tcap)) {
                memcpy(u, tu, sizeof u);
                cap = tcap;
                hit = 1;
                break;
            }
        }
        if (td)
            closedir(td);
        if (hit)
            report(atol(de->d_name), name, u[0], u[1], u[2], u[3], cap);
    }
    closedir(d);
    return 0;
}
#endif

int main(void)
{
    if (scan() < 0) {
        printf("%%ROOTAUDIT-F-NOLIST, cannot read the substrate process list\n");
        return 44;
    }
    printf("%%ROOTAUDIT-I-SUMMARY, %u process(es) run as substrate root\n", n_root);
    fflush(stdout);
    return n_root ? (int)STS_ROOTSEEN : (int)STS_NORMAL;
}
