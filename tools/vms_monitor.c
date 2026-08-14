/*
 * vms_monitor.c - OVMX MONITOR Utility
 *
 * Displays real-time system performance statistics in VMS MONITOR format.
 * Supports SYSTEM, PROCESSES, DISK, and IO subcommands.
 *
 * Data sources:
 *   CPU:      /proc/stat
 *   Memory:   /proc/meminfo
 *   Disk:     /proc/diskstats
 *   Process:  the EXECUTIVE process table, via vms_kif_procscan()
 *             (src/libvmssys/vms_kif.c) -- the SAME single source DCL
 *             SHOW SYSTEM reads (src/vmsdcl/dcl_cmd_show.c). See the
 *             process-reader section below for why MONITOR must NOT keep
 *             its own /proc scan (vms-c840).
 *
 * Usage:
 *   vms_monitor [SYSTEM|PROCESSES|DISK|IO]
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include "ovmx_identity.h"
#include "vms_kif.h"   /* vms_kif_procscan + struct vms_procinfo (vms-c840) */

/* ================================================================== */
/*                         Constants                                   */
/* ================================================================== */

#define REFRESH_SECS     3
#define MAX_PROCS        64
#define BAR_WIDTH        23     /* chars inside the bar brackets */
#define NODE_NAME        "OVMX"
/* Identity comes from the SSOT (INV-1); MONITOR's header column is tight,
 * so it uses the unbadged brand id. */
#define OVMX_VERSION     OVMX_PRODUCT_ID

/* ================================================================== */
/*                         Data Types                                  */
/* ================================================================== */

/* Subcommand enum */
typedef enum {
    MON_SYSTEM = 0,
    MON_PROCESSES,
    MON_DISK,
    MON_IO,
} mon_mode_t;

/* Rolling stats: track cur/avg/min/max over session */
typedef struct {
    double cur;
    double avg;
    double min;
    double max;
    long   samples;
    double sum;
} stat_t;

/* CPU raw counters from /proc/stat */
typedef struct {
    unsigned long long user;
    unsigned long long nice;
    unsigned long long system;
    unsigned long long idle;
    unsigned long long iowait;
    unsigned long long irq;
    unsigned long long softirq;
    unsigned long long steal;
} cpu_raw_t;

/* Memory statistics (in pages; page = 4KB) */
typedef struct {
    long mem_total_kb;
    long mem_free_kb;
    long buffers_kb;
    long cached_kb;
    long swap_total_kb;
    long swap_free_kb;
} mem_info_t;

/* Disk I/O counters (aggregated across all block devices) */
typedef struct {
    unsigned long long reads;
    unsigned long long writes;
    unsigned long long read_sectors;
    unsigned long long write_sectors;
} disk_raw_t;

/* Per-process info, mapped from the executive's struct vms_procinfo row
 * (vms-c840). A field the executive does not source (VMS state, VMS
 * priority) carries a `has_*` flag CLEAR and is rendered absent, never as a
 * fabricated value -- the same discipline SHOW SYSTEM applies to the same
 * row (docs/oracle/vax73-show-system-process.md sec 5.1). */
typedef struct {
    int    pid;             /* JPI$_PID (vms_procinfo.vms_pid) */
    char   name[64];        /* process name (vms_procinfo.prcnam), "" unnamed */
    char   vms_state[8];    /* VMS scheduler state -- "" (not executive-sourced) */
    int    has_state;
    int    priority;        /* VMS priority (vms_procinfo.cur_pri) if sourced */
    int    has_pri;
    char   cpu_str[24];     /* "HH:MM:SS.CC" from JPI$_CPUTIM, "" if absent */
    int    has_cpu;
    long   minflt;          /* JPI$_PAGEFLTS (vms_procinfo.pageflts) if sourced */
    int    has_pgflt;
} proc_info_t;

/* Session-level rolling stats */
typedef struct {
    stat_t cpu_busy;
    stat_t cpu_interrupt;
    stat_t proc_count;
    stat_t page_faults;
    stat_t pages_io;
    stat_t free_list;
    stat_t mod_list;
    stat_t disk_reads;
    stat_t disk_writes;
    stat_t io_ops;
} session_stats_t;

/* ================================================================== */
/*                      Global State                                   */
/* ================================================================== */

static volatile int g_quit = 0;
static struct termios g_orig_termios;
static int g_termios_saved = 0;

/* ================================================================== */
/*                      Signal Handling                                */
/* ================================================================== */

static void sig_handler(int sig)
{
    (void)sig;
    g_quit = 1;
}

/* ================================================================== */
/*                      Terminal Helpers                               */
/* ================================================================== */

static void term_raw(void)
{
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == 0) {
        g_termios_saved = 1;
        t = g_orig_termios;
        t.c_lflag &= ~(unsigned)(ICANON | ECHO);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
}

static void term_restore(void)
{
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    }
}

static void clear_screen(void)
{
    /* Move cursor to top-left and clear screen */
    fputs("\033[H\033[2J", stdout);
}

/* Check if 'Q' has been pressed (non-blocking) */
static int key_pressed_quit(void)
{
    char c = 0;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1 && (c == 'q' || c == 'Q')) return 1;
    return 0;
}

/* ================================================================== */
/*                      VMS Date/Time Format                           */
/* ================================================================== */

static const char *vms_months[] = {
    "JAN","FEB","MAR","APR","MAY","JUN",
    "JUL","AUG","SEP","OCT","NOV","DEC"
};

static void vms_datetime(char *buf, size_t len)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    int cc = (int)(ts.tv_nsec / 10000000);
    snprintf(buf, len, "%02d-%s-%04d %02d:%02d:%02d.%02d",
             tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
             tm.tm_hour, tm.tm_min, tm.tm_sec, cc);
}

/* Format ticks (USER_HZ = 100) into "HH:MM:SS.CC" */
static void ticks_to_vms_time(long ticks, char *buf, size_t len)
{
    /* USER_HZ is 100 on Linux */
    long centisecs = ticks;
    long secs = centisecs / 100;
    long cc   = centisecs % 100;
    long mins = secs / 60;
    secs %= 60;
    long hours = mins / 60;
    mins %= 60;
    snprintf(buf, len, "%02ld:%02ld:%02ld.%02ld", hours, mins, secs, cc);
}

/* ================================================================== */
/*                      Stat Helpers                                   */
/* ================================================================== */

static void stat_update(stat_t *s, double val)
{
    s->cur = val;
    s->samples++;
    s->sum += val;
    s->avg = s->sum / (double)s->samples;
    if (s->samples == 1) {
        s->min = val;
        s->max = val;
    } else {
        if (val < s->min) s->min = val;
        if (val > s->max) s->max = val;
    }
}

/* Print a bar graph row:
 * "Label (range) CUR AVG MIN MAX  |####.......|"
 *
 * label_width: field width for label
 * val is 0..100 for percentage bars; range is max_val for scaling
 */
static void print_bar_row(const char *label, int label_width,
                          const char *range_str,
                          stat_t *s, double bar_max)
{
    /* Numeric columns: right-aligned in width 4 */
    printf("%-*s %s %4.0f    %4.0f    %4.0f    %4.0f  |",
           label_width, label,
           range_str,
           s->cur, s->avg, s->min, s->max);

    /* Draw bar proportional to cur value */
    double frac = (bar_max > 0.0) ? (s->cur / bar_max) : 0.0;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    int filled = (int)(frac * BAR_WIDTH);
    for (int i = 0; i < BAR_WIDTH; i++) {
        putchar(i < filled ? '*' : ' ');
    }
    puts("|");
}

/* Print a non-bar numeric row */
static void print_num_row(const char *label, int label_width,
                          stat_t *s)
{
    printf("%-*s      %6.0f  %6.0f  %6.0f  %6.0f\n",
           label_width, label,
           s->cur, s->avg, s->min, s->max);
}

/* ================================================================== */
/*                      /proc Readers                                  */
/* ================================================================== */

static int read_cpu_raw(cpu_raw_t *c)
{
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return -1;

    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu ", 4) == 0) {
            sscanf(line + 4,
                   "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &c->user, &c->nice, &c->system, &c->idle,
                   &c->iowait, &c->irq, &c->softirq, &c->steal);
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found ? 0 : -1;
}

/* Returns CPU busy % and interrupt % from two samples */
static void cpu_delta_percent(const cpu_raw_t *prev, const cpu_raw_t *cur,
                              double *busy_pct, double *irq_pct)
{
    unsigned long long total_prev = prev->user + prev->nice + prev->system +
                                    prev->idle + prev->iowait + prev->irq +
                                    prev->softirq + prev->steal;
    unsigned long long total_cur  = cur->user + cur->nice + cur->system +
                                    cur->idle + cur->iowait + cur->irq +
                                    cur->softirq + cur->steal;

    unsigned long long delta_total = total_cur - total_prev;
    if (delta_total == 0) {
        *busy_pct = 0.0;
        *irq_pct = 0.0;
        return;
    }

    unsigned long long delta_idle  = cur->idle - prev->idle;
    unsigned long long delta_irq   = (cur->irq + cur->softirq) -
                                     (prev->irq + prev->softirq);

    *busy_pct = 100.0 * (double)(delta_total - delta_idle) / (double)delta_total;
    *irq_pct  = 100.0 * (double)delta_irq / (double)delta_total;
}

static int read_meminfo(mem_info_t *m)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1;

    memset(m, 0, sizeof(*m));
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        long val = 0;
        if (sscanf(line, "MemTotal: %ld kB", &val) == 1)
            m->mem_total_kb = val;
        else if (sscanf(line, "MemFree: %ld kB", &val) == 1)
            m->mem_free_kb = val;
        else if (sscanf(line, "Buffers: %ld kB", &val) == 1)
            m->buffers_kb = val;
        else if (sscanf(line, "Cached: %ld kB", &val) == 1)
            m->cached_kb = val;
        else if (sscanf(line, "SwapTotal: %ld kB", &val) == 1)
            m->swap_total_kb = val;
        else if (sscanf(line, "SwapFree: %ld kB", &val) == 1)
            m->swap_free_kb = val;
    }
    fclose(fp);
    return 0;
}

/*
 * Process count and aggregate page faults come from the EXECUTIVE process
 * table (vms-c840), enumerated with the same vms_kif_procscan() cursor
 * read_processes() below and DCL SHOW SYSTEM use. A /proc walk here would
 * be a second, divergent source that also counts Linux tasks the VMS view
 * does not own -- the exact defect this item fixes.
 */
static int count_procs(void)
{
    uint32_t index = 0;
    struct vms_procinfo info;
    int count = 0;
    while (vms_kif_procscan(&index, &info) & 1)
        count++;
    return count;
}

/* Sum page faults across the executive's processes (JPI$_PAGEFLTS per row,
 * only where the executive sourced it -- absent is not counted as zero). */
static long sum_page_faults(void)
{
    uint32_t index = 0;
    struct vms_procinfo info;
    long total = 0;
    while (vms_kif_procscan(&index, &info) & 1) {
        if (info.fields_valid & VMS_PI_V_PAGEFLTS)
            total += (long)info.pageflts;
    }
    return total;
}

static int read_diskstats(disk_raw_t *d)
{
    FILE *fp = fopen("/proc/diskstats", "r");
    if (!fp) return -1;

    memset(d, 0, sizeof(*d));
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        unsigned int major, minor;
        char devname[64];
        unsigned long long reads, reads_merged, read_sectors, read_ms;
        unsigned long long writes, writes_merged, write_sectors, write_ms;

        int n = sscanf(line,
            "%u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu",
            &major, &minor, devname,
            &reads, &reads_merged, &read_sectors, &read_ms,
            &writes, &writes_merged, &write_sectors, &write_ms);

        if (n < 11) continue;

        /* Skip loop, ram, and partition devices (only aggregate whole disks) */
        if (strncmp(devname, "loop", 4) == 0) continue;
        if (strncmp(devname, "ram",  3) == 0) continue;

        /* Skip partitions: names ending in digit after a non-digit prefix
         * e.g. sda1 but not sda. We detect by: if name ends in digit AND
         * there is a letter before the trailing digits, it is a partition.
         */
        size_t dlen = strlen(devname);
        if (dlen > 0 && isdigit((unsigned char)devname[dlen - 1])) {
            /* Check if the character before trailing digits is a letter */
            size_t i = dlen;
            while (i > 0 && isdigit((unsigned char)devname[i - 1])) i--;
            if (i > 0 && isalpha((unsigned char)devname[i - 1])) continue;
        }

        d->reads        += reads;
        d->writes       += writes;
        d->read_sectors += read_sectors;
        d->write_sectors+= write_sectors;
    }
    fclose(fp);
    return 0;
}

/*
 * Read per-process info from the EXECUTIVE process table (vms-c840) --
 * returns the count of processes read.
 *
 * THIS IS THE WHOLE FIX. MONITOR used to build its process list by walking
 * the /proc directory and each numeric per-PID stat entry under it, which
 * enumerates EVERY Linux task -- including Linux's own kernel threads and
 * daemons -- while DCL SHOW
 * SYSTEM reads the executive's process table through vms_kif_procscan(). Two
 * different sources answered the same VMS question with two different lists
 * (operator-observed 2026-08-14). MONITOR now reads the SAME single source
 * SHOW SYSTEM does, so the two commands list an IDENTICAL set of processes
 * -- whatever the executive's table holds. There is no filtering here and no
 * second source: the set is exactly what the executive returns.
 *
 * Field mapping mirrors dcl_cmd_show.c's cmd_show_system() row-for-row. A
 * field the executive sources (name, JPI$_CPUTIM, JPI$_PAGEFLTS) is rendered
 * at its real value; a field it does NOT source (VMS scheduler state,
 * VMS priority) is marked absent and rendered blank -- never invented from a
 * Linux value, the fabrication class the SHOW SYSTEM oracle refused
 * (docs/oracle/vax73-show-system-process.md sec 5.1).
 *
 * INV-6 / Rule 9: /dev/vms is the transport; if it is absent vms_kif_procscan
 * returns a failure status (odd-bit clear) on the first call, the loop makes
 * no iterations, and MONITOR shows no processes -- it does NOT fall back to
 * /proc. In the real runtime PID 1 refuses to boot without the executive, so
 * the empty case only arises off the runtime (e.g. host CI), exactly as it
 * does for SHOW SYSTEM.
 */
static int read_processes(proc_info_t *procs, int max_procs)
{
    uint32_t index = 0;
    struct vms_procinfo info;
    int count = 0;

    while (count < max_procs && (vms_kif_procscan(&index, &info) & 1)) {
        proc_info_t *p = &procs[count++];
        memset(p, 0, sizeof(*p));
        p->pid = (int)info.vms_pid;

        /* Process name as the executive holds it (prcnam), uppercased for the
         * VMS display column exactly as the old path uppercased the comm. */
        size_t ci;
        for (ci = 0; info.prcnam[ci] && ci < sizeof(p->name) - 1; ci++)
            p->name[ci] = (char)toupper((unsigned char)info.prcnam[ci]);
        p->name[ci] = '\0';

        /* CPU time: JPI$_CPUTIM, 10ms units == centiseconds, the same unit
         * ticks_to_vms_time() already formats (USER_HZ 100). Absent -> "". */
        if (info.fields_valid & VMS_PI_V_CPUTIM) {
            ticks_to_vms_time((long)info.cputim, p->cpu_str, sizeof(p->cpu_str));
            p->has_cpu = 1;
        }

        /* Page faults: JPI$_PAGEFLTS. Absent -> not shown (not a zero). */
        if (info.fields_valid & VMS_PI_V_PAGEFLTS) {
            p->minflt = (long)info.pageflts;
            p->has_pgflt = 1;
        }

        /* VMS priority: only if the executive sources it (it does not yet --
         * VMS_PI_V_PRI clear). No Linux nice-value substitution. */
        if (info.fields_valid & VMS_PI_V_PRI) {
            p->priority = (int)info.cur_pri;
            p->has_pri = 1;
        }

        /* VMS scheduler state: the executive has no VMS scheduler, so this is
         * absent (VMS_PI_V_STATE clear) and shown blank -- never mapped from a
         * Linux task state, which is the divergence SHOW SYSTEM also refuses. */
        if (info.fields_valid & VMS_PI_V_STATE) {
            /* Structural: no VMS state string table is defined until the
             * executive sources one; leave blank rather than invent. */
            p->has_state = 1;
        }
    }
    return count;
}

/* ================================================================== */
/*                      Display: SYSTEM                                */
/* ================================================================== */

static void display_system(session_stats_t *ss, int sample_num)
{
    char dtbuf[32];
    vms_datetime(dtbuf, sizeof(dtbuf));

    clear_screen();
    printf("                          OVMX Monitor Utility\n");
    printf("                              %s\n", OVMX_VERSION);
    printf("                    on node %-8s  %s\n\n", NODE_NAME, dtbuf);
    printf("                          SYSTEM STATISTICS\n\n");

    if (sample_num < 2) {
        printf("                    Collecting initial sample...\n");
        return;
    }

    /* Column headers */
    printf("%-26s        CUR     AVG     MIN     MAX\n", "");
    printf("%-26s   (0-100)\n", "CPU Busy");
    print_bar_row("CPU Busy", 18, "(0-100)",
                  &ss->cpu_busy, 100.0);
    print_bar_row("Interrupt State", 18, "(0-100)",
                  &ss->cpu_interrupt, 100.0);

    printf("\n");
    print_num_row("Processes", 30, &ss->proc_count);
    print_num_row("Page Faults (per sec)", 30, &ss->page_faults);
    print_num_row("Pages In I/O (per sec)", 30, &ss->pages_io);
    print_num_row("Free List Size", 30, &ss->free_list);
    print_num_row("Modified List Size", 30, &ss->mod_list);

    /* Top processes */
    printf("\nCur Top Processes\n");
    proc_info_t procs[MAX_PROCS];
    int nproc = read_processes(procs, MAX_PROCS);

    /* Show up to 8 processes */
    int shown = 0;
    for (int i = 0; i < nproc && shown < 8; i++) {
        printf("%-16s (state %-4s, CPU %s)\n",
               procs[i].name, procs[i].vms_state, procs[i].cpu_str);
        shown++;
    }
    if (shown == 0) {
        printf("  (no processes visible)\n");
    }

    printf("\nPress Q to exit, refreshing every %d seconds...\n", REFRESH_SECS);
    fflush(stdout);
}

/* ================================================================== */
/*                      Display: PROCESSES                             */
/* ================================================================== */

static void display_processes(void)
{
    char dtbuf[32];
    vms_datetime(dtbuf, sizeof(dtbuf));

    clear_screen();
    printf("                         PROCESS STATISTICS\n");
    printf("                                              %s\n\n", dtbuf);
    printf("%-20s  %-8s  %-6s  %3s  %-16s  %s\n",
           "Process Name", "PID", "State", "Pri", "CPU Time", "Page Flts");
    printf("%-20s  %-8s  %-6s  %3s  %-16s  %s\n",
           "------------", "--------", "-----", "---", "--------", "---------");

    proc_info_t procs[MAX_PROCS];
    int nproc = read_processes(procs, MAX_PROCS);

    for (int i = 0; i < nproc; i++) {
        /* Absent (non-executive-sourced) fields render as blanks of the
         * column width, never as a fabricated 0 -- the same "absent is not
         * zero" discipline SHOW SYSTEM applies to this executive row. */
        char pri_str[8];
        char pf_str[16];
        if (procs[i].has_pri) snprintf(pri_str, sizeof(pri_str), "%3d", procs[i].priority);
        else                  snprintf(pri_str, sizeof(pri_str), "%3s", "");
        if (procs[i].has_pgflt) snprintf(pf_str, sizeof(pf_str), "%ld", procs[i].minflt);
        else                    pf_str[0] = '\0';

        printf("%-20s  %08X  %-6s  %s  %-16s  %s\n",
               procs[i].name,
               (unsigned int)procs[i].pid,
               procs[i].vms_state,
               pri_str,
               procs[i].cpu_str,
               pf_str);
    }
    if (nproc == 0) {
        printf("  (no processes visible)\n");
    }

    printf("\nPress Q to exit, refreshing every %d seconds...\n", REFRESH_SECS);
    fflush(stdout);
}

/* ================================================================== */
/*                      Display: DISK                                  */
/* ================================================================== */

static void display_disk(session_stats_t *ss, int sample_num)
{
    char dtbuf[32];
    vms_datetime(dtbuf, sizeof(dtbuf));

    clear_screen();
    printf("                          OVMX Monitor Utility\n");
    printf("                              %s\n", OVMX_VERSION);
    printf("                    on node %-8s  %s\n\n", NODE_NAME, dtbuf);
    printf("                           DISK STATISTICS\n\n");

    if (sample_num < 2) {
        printf("                    Collecting initial sample...\n");
        return;
    }

    printf("%-30s        CUR     AVG     MIN     MAX\n", "");
    print_num_row("Disk Reads (per sec)", 30, &ss->disk_reads);
    print_num_row("Disk Writes (per sec)", 30, &ss->disk_writes);

    printf("\nPress Q to exit, refreshing every %d seconds...\n", REFRESH_SECS);
    fflush(stdout);
}

/* ================================================================== */
/*                      Display: IO                                    */
/* ================================================================== */

static void display_io(session_stats_t *ss, int sample_num)
{
    char dtbuf[32];
    vms_datetime(dtbuf, sizeof(dtbuf));

    clear_screen();
    printf("                          OVMX Monitor Utility\n");
    printf("                              %s\n", OVMX_VERSION);
    printf("                    on node %-8s  %s\n\n", NODE_NAME, dtbuf);
    printf("                           I/O STATISTICS\n\n");

    if (sample_num < 2) {
        printf("                    Collecting initial sample...\n");
        return;
    }

    printf("%-30s        CUR     AVG     MIN     MAX\n", "");
    print_num_row("Direct I/O Rate (per sec)", 30, &ss->io_ops);
    print_num_row("Disk Reads (per sec)", 30, &ss->disk_reads);
    print_num_row("Disk Writes (per sec)", 30, &ss->disk_writes);
    print_num_row("Page Faults (per sec)", 30, &ss->page_faults);
    print_num_row("Free List Size", 30, &ss->free_list);

    printf("\nPress Q to exit, refreshing every %d seconds...\n", REFRESH_SECS);
    fflush(stdout);
}

/* ================================================================== */
/*                      Main Monitor Loop                              */
/* ================================================================== */

static void run_monitor(mon_mode_t mode)
{
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    term_raw();

    session_stats_t ss;
    memset(&ss, 0, sizeof(ss));

    cpu_raw_t cpu_prev, cpu_cur;
    memset(&cpu_prev, 0, sizeof(cpu_prev));
    memset(&cpu_cur,  0, sizeof(cpu_cur));

    disk_raw_t disk_prev, disk_cur;
    memset(&disk_prev, 0, sizeof(disk_prev));
    memset(&disk_cur,  0, sizeof(disk_cur));

    long pflt_prev = 0;
    int  sample_num = 0;

    /* Initial readings */
    read_cpu_raw(&cpu_prev);
    read_diskstats(&disk_prev);
    pflt_prev = sum_page_faults();

    while (!g_quit) {
        /* Sleep REFRESH_SECS, checking for keypress every 100ms */
        for (int ms = 0; ms < REFRESH_SECS * 10 && !g_quit; ms++) {
            usleep(100000); /* 100ms */
            if (key_pressed_quit()) { g_quit = 1; break; }
        }
        if (g_quit) break;

        /* Collect new samples */
        read_cpu_raw(&cpu_cur);
        read_diskstats(&disk_cur);
        long pflt_cur = sum_page_faults();

        double busy_pct = 0.0, irq_pct = 0.0;
        cpu_delta_percent(&cpu_prev, &cpu_cur, &busy_pct, &irq_pct);

        mem_info_t mem;
        read_meminfo(&mem);

        int nproc = count_procs();

        /* Page faults per second (delta / refresh period) */
        long pflt_delta = pflt_cur - pflt_prev;
        double pflt_rate = (double)pflt_delta / REFRESH_SECS;

        /* Disk ops per second */
        double disk_reads_rate  = (double)(disk_cur.reads  - disk_prev.reads)  / REFRESH_SECS;
        double disk_writes_rate = (double)(disk_cur.writes - disk_prev.writes) / REFRESH_SECS;
        double io_ops_rate      = disk_reads_rate + disk_writes_rate;

        /* Memory: free list in pages (4KB), modified list estimated from buffers */
        long free_pages = mem.mem_free_kb / 4;
        long mod_pages  = (mem.buffers_kb + mem.cached_kb) / 4;
        /* Clamp to reasonable "pages in I/O" estimate */
        double pages_io = (double)((disk_cur.read_sectors - disk_prev.read_sectors) / 8) / REFRESH_SECS;

        /* Update rolling stats */
        stat_update(&ss.cpu_busy,      busy_pct);
        stat_update(&ss.cpu_interrupt, irq_pct);
        stat_update(&ss.proc_count,    (double)nproc);
        stat_update(&ss.page_faults,   pflt_rate);
        stat_update(&ss.pages_io,      pages_io);
        stat_update(&ss.free_list,     (double)free_pages);
        stat_update(&ss.mod_list,      (double)mod_pages);
        stat_update(&ss.disk_reads,    disk_reads_rate);
        stat_update(&ss.disk_writes,   disk_writes_rate);
        stat_update(&ss.io_ops,        io_ops_rate);

        sample_num++;

        /* Render selected display */
        switch (mode) {
            case MON_SYSTEM:
                display_system(&ss, sample_num);
                break;
            case MON_PROCESSES:
                display_processes();
                break;
            case MON_DISK:
                display_disk(&ss, sample_num);
                break;
            case MON_IO:
                display_io(&ss, sample_num);
                break;
        }

        /* Rotate prev */
        cpu_prev   = cpu_cur;
        disk_prev  = disk_cur;
        pflt_prev  = pflt_cur;
    }

    term_restore();
    /* Restore cursor / clear line */
    printf("\n\n  MONITOR exited.\n");
}

/* ================================================================== */
/*                      Entry Point                                    */
/* ================================================================== */

int main(int argc, char *argv[])
{
    mon_mode_t mode = MON_SYSTEM;

    if (argc >= 2) {
        const char *sub = argv[1];

        /* Case-insensitive prefix match (min 2 chars) */
        if (strncasecmp(sub, "PROCESSES", 3) == 0 ||
            strncasecmp(sub, "PROC",      4) == 0)
            mode = MON_PROCESSES;
        else if (strncasecmp(sub, "DISK", 4) == 0 ||
                 strncasecmp(sub, "DI",   2) == 0)
            mode = MON_DISK;
        else if (strncasecmp(sub, "IO",   2) == 0 ||
                 strncasecmp(sub, "I/O",  3) == 0)
            mode = MON_IO;
        else if (strncasecmp(sub, "SYSTEM", 3) == 0 ||
                 strncasecmp(sub, "SYS",    3) == 0)
            mode = MON_SYSTEM;
        else {
            fprintf(stderr,
                    "%%MONITOR-E-IVSUBCMD, unrecognized subcommand '%s'\n"
                    "  Valid subcommands: SYSTEM, PROCESSES, DISK, IO\n",
                    sub);
            return 1;
        }
    }

    run_monitor(mode);
    return 0;
}
