/*
 * sys_time.c - Time System Services
 *
 * VMS time is a 64-bit value representing 100-nanosecond intervals
 * since November 17, 1858 00:00:00.00 (the Smithsonian Modified Julian
 * Date base). This module converts between VMS and Unix time, and
 * implements the standard VMS time system services.
 *
 * VMS epoch: November 17, 1858
 * Unix epoch: January 1, 1970
 * Offset: 3,506,716,800 seconds = 0x007C95674BEB4000 in 100ns ticks
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * THE SEVEN SERVICES HERE CITE TWO ITEMS, NOT ONE (vms-fab). They all used to
 * cite vms-5b4, the item that BUILT this register; it is closed and owned none
 * of them. They divide by what their answer depends on:
 *
 *   vms-642  the four that want a system the executive owns -- a system-time
 *            cell two processes can both see, and a timer queue something other
 *            than this image can fire.
 *   vms-f90  the three that transform the caller's own arguments and read no
 *            system state, alongside $FAO and the other compute-only services.
 *            Whether OpenVMS answers those in process context is UNPINNED; that
 *            item's outcome is the pin, not a rewrite.
 *
 * OVMX-USERSPACE: sys$gettim (vms-642) -- clock_gettime(CLOCK_REALTIME)
 *     converted to VMS 100ns ticks; the host clock, not an executive EXE$GQ_
 *     system time cell, so no system-time base can be set or observed.
 * OVMX-PARTIAL: sys$getutc (vms-44a) -- exec: the system TDF is the executive's
 *     SYS$TIMEZONE_DIFFERENTIAL logical, read through $TRNLNM.
 * OVMX-LOCAL: sys$getutc -- the same host clock read, laid out as the
 *     16-byte $UTCDEF structure (abstime, unspecified inaccuracy, TDF word).
 * OVMX-PARTIAL: sys$binutc (vms-44a) -- exec: the system TDF is the executive's
 *     SYS$TIMEZONE_DIFFERENTIAL logical, read through $TRNLNM.
 * OVMX-LOCAL: sys$binutc -- sys$bintim on the caller's string, then the
 *     local -> UTC step with the system TDF; the TDF is the SYS$TIMEZONE_DIFFERENTIAL
 *     logical read through $TRNLNM. Observed on the Alpha V8.4 lab at TDF 0 only.
 * OVMX-PARTIAL: sys$numutc (vms-44a) -- exec: the system TDF is the executive's
 *     SYS$TIMEZONE_DIFFERENTIAL logical, read through $TRNLNM.
 * OVMX-LOCAL: sys$numutc -- $TIMCON then $NUMTIM on the caller's structure.
 * OVMX-PARTIAL: sys$ascutc (vms-44a) -- exec: the system TDF is the executive's
 *     SYS$TIMEZONE_DIFFERENTIAL logical, read through $TRNLNM.
 * OVMX-LOCAL: sys$ascutc -- $TIMCON then $ASCTIM; cvtflg bit 0 = time only.
 * OVMX-PARTIAL: sys$timcon (vms-44a) -- exec: the system TDF is the executive's
 *     SYS$TIMEZONE_DIFFERENTIAL logical, read through $TRNLNM.
 * OVMX-LOCAL: sys$timcon -- UTC <-> system quadword arithmetic on the
 *     caller's structure; the system TDF is the logical above.
 * OVMX-USERSPACE: sys$gettim_prec (vms-642) -- the same host clock read; the
 *     host clock already has sub-tick resolution, so it never reports
 *     reduced precision.
 * OVMX-USERSPACE: sys$numtim (vms-f90) -- converts the caller's quadword (or
 *     the host clock when timadr is NULL) into the caller's timbuf.
 * OVMX-USERSPACE: sys$asctim (vms-f90) -- formats into the caller's buffer
 *     from the compiled-in months[] table in this file.
 * OVMX-USERSPACE: sys$bintim (vms-f90) -- parses the caller's string against
 *     that same compiled-in table.
 * OVMX-PARTIAL: sys$schdwk (vms-44a) -- exec: the wake state, which the timer's
 *     expiry AST sets with $WAKE (an AST function pointer this register's static
 *     call graph cannot follow); the request goes through sys$setimr with
 *     EFN$C_ENF, so no event flag is cleared or set (rd vms-8d1).
 * OVMX-LOCAL: sys$schdwk -- the schedule is this process's own POSIX timer (the
 *     sys$setimr table below): a scheduled wakeup dies with the image that
 *     requested it and no executive timer queue holds it. A repeat interval, or
 *     a target named by process name, is refused.
 * OVMX-USERSPACE: sys$canwak (vms-44a) -- cancels the timers sys$schdwk armed in
 *     that same process-local table.
 * OVMX-PARTIAL: sys$setimr (vms-d08) -- exec: the event flag. It is cleared
 *     when the request is queued, through $CLREF, whose executive answer also
 *     validates the efn (SS$_UNASEFC / SS$_ILLEFC fail the request), and set by
 *     $SETEF at expiry -- as the semantic oracle observed on real VAX V7.3 and
 *     Alpha V8.4 (docs/oracle/semantics/ef/, EF.SETIMR.*).
 * OVMX-LOCAL: sys$setimr -- (vms-642) the timer itself is a POSIX timer recorded
 *     in the process-local timer_table[] in this file. There is no executive
 *     timer queue, so the request dies with the process and nothing else can
 *     see it.
 * OVMX-USERSPACE: sys$cantim (vms-642) -- cancels entries in that same
 *     process-local table, so the timers it cancels are this process's.
 */

#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <strings.h>
#include <pthread.h>
#include "starlet.h"
#include "gen64def.h"   /* struct _generic_64 for sys$bintim's timadr */
#include "efndef.h"     /* EFN$C_ENF */
#include "ovmx_utc.h"   /* ovmx_timegm: no C RTL timegm on OpenVMS */

/* The per-request host timers below are POSIX timers (timer_create), which the
 * OpenVMS C RTL does not have, and their expiry runs a handler the kernel
 * enters directly -- not through a procedure descriptor of the OpenVMS Alpha
 * calling standard. So a build for that standard (the OVMX/Alpha shareables a
 * native VMS image calls) queues its timer requests on ONE interval timer
 * instead (vms_timer_* below), whose SIGALRM enters the handler through a
 * small code thunk that loads the handler's procedure descriptor. */
#if (defined(__alpha) || defined(__alpha__)) && (defined(__VMS) || defined(__vms) || defined(__VMS__))
#define OVMX_HOST_POSIX_TIMERS 0
#else
#define OVMX_HOST_POSIX_TIMERS 1
#endif

/* Offset between VMS epoch (Nov 17 1858) and Unix epoch (Jan 1 1970) in 100ns units */
#define VMS_EPOCH_OFFSET 0x007C95674BEB4000ULL

/* ---- Conversion helpers ---- */

/*
 * unix_to_vms_time - Convert a Unix timespec to VMS 64-bit time.
 */
static uint64_t unix_to_vms_time(const struct timespec *ts) {
    uint64_t vmstime = (uint64_t)ts->tv_sec * 10000000ULL;
    vmstime += (uint64_t)ts->tv_nsec / 100;
    vmstime += VMS_EPOCH_OFFSET;
    return vmstime;
}

/*
 * vms_to_unix_time - Convert a VMS 64-bit time to Unix timespec.
 */
static int vms_to_unix_time(uint64_t vmstime, struct timespec *ts) {
    if (vmstime < VMS_EPOCH_OFFSET) return -1;  /* Pre-Unix date */
    vmstime -= VMS_EPOCH_OFFSET;
    ts->tv_sec = (time_t)(vmstime / 10000000ULL);
    ts->tv_nsec = (long)((vmstime % 10000000ULL) * 100);
    return 0;
}

/* ---- System Services ---- */

/*
 * sys$gettim - Get current system time in VMS format.
 *
 * Uses clock_gettime(CLOCK_REALTIME) for nanosecond precision,
 * then converts to VMS 100-nanosecond ticks.
 */
uint32_t sys$gettim(uint64_t *timadr) {
    if (!timadr) return SS$_BADPARAM;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *timadr = unix_to_vms_time(&ts);

    return SS$_NORMAL;
}

/*
 * sys$numtim - Convert VMS binary time to 7-word numeric buffer.
 *
 * Output:
 *   timbuf[0] = year   (e.g. 2026)
 *   timbuf[1] = month  (1-12)
 *   timbuf[2] = day    (1-31)
 *   timbuf[3] = hour   (0-23)
 *   timbuf[4] = minute (0-59)
 *   timbuf[5] = second (0-59)
 *   timbuf[6] = hundredths of a second (0-99)
 *
 * If timadr is NULL, the current time is used.
 */
uint32_t sys$numtim(uint16_t timbuf[7], const uint64_t *timadr) {
    if (!timbuf) return SS$_BADPARAM;

    struct timespec ts;
    if (timadr) {
        if (vms_to_unix_time(*timadr, &ts) < 0) return SS$_BADPARAM;
    } else {
        clock_gettime(CLOCK_REALTIME, &ts);
    }

    struct tm tm_result;
    gmtime_r(&ts.tv_sec, &tm_result);

    timbuf[0] = (uint16_t)(tm_result.tm_year + 1900);
    timbuf[1] = (uint16_t)(tm_result.tm_mon + 1);
    timbuf[2] = (uint16_t)tm_result.tm_mday;
    timbuf[3] = (uint16_t)tm_result.tm_hour;
    timbuf[4] = (uint16_t)tm_result.tm_min;
    timbuf[5] = (uint16_t)tm_result.tm_sec;
    timbuf[6] = (uint16_t)(ts.tv_nsec / 10000000);  /* ns -> hundredths */

    return SS$_NORMAL;
}

/*
 * sys$asctim - Convert VMS binary time to ASCII string.
 *
 * VMS format: "DD-MMM-YYYY HH:MM:SS.CC"  (23 characters)
 */
uint32_t sys$asctim(uint16_t *timlen, struct dsc$descriptor_s *timbuf,
                    const uint64_t *timadr, uint32_t cvtflg) {
    if (!timbuf || !timbuf->dsc$a_pointer) return SS$_BADPARAM;

    static const char *months[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };

    uint16_t numtim[7];
    sys$numtim(numtim, timadr);

    /* Validate month range to prevent OOB access */
    if (numtim[1] < 1 || numtim[1] > 12) return SS$_BADPARAM;

    char buf[24];
    int len;
    if (cvtflg & 1)   /* bit 0: the time only (observed on OpenVMS Alpha V8.4 for $ASCUTC) */
        len = snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%02d",
                       numtim[3], numtim[4], numtim[5], numtim[6]);
    else
        len = snprintf(buf, sizeof(buf), "%2d-%s-%04d %02d:%02d:%02d.%02d",
                       numtim[2], months[numtim[1] - 1], numtim[0],
                       numtim[3], numtim[4], numtim[5], numtim[6]);

    uint16_t copylen = (uint16_t)len;
    if (copylen > timbuf->dsc$w_length) copylen = timbuf->dsc$w_length;
    memcpy(timbuf->dsc$a_pointer, buf, copylen);
    if (timlen) *timlen = copylen;

    return SS$_NORMAL;
}

/*
 * sys$bintim - Convert ASCII time string to VMS binary time.
 *
 * Parses the VMS date/time format "DD-MMM-YYYY HH:MM:SS.CC"
 * and converts it to a VMS 64-bit binary time value.
 */
uint32_t sys$bintim(const struct dsc$descriptor_s *timbuf,
                    struct _generic_64 *timadr) {
    if (!timbuf || !timadr || !timbuf->dsc$a_pointer) return SS$_BADPARAM;
    /* timadr is the address of the quadword that receives the converted time
     * (VSI System Services Reference, $BINTIM). */
    uint64_t *q = &timadr->gen64$q_quadword;

    char buf[64];
    dsc$strncpy(buf, timbuf, sizeof(buf));

    static const char *months[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };

    int day, year, hour = 0, min = 0, sec = 0, hun = 0;
    char mon[4] = {0};

    /*
     * A VMS delta time has the form "[dddd ]hh:mm:ss[.cc]" with no month
     * name — e.g. "1 01:00:00.00" (1 day, 1 hour).  It is stored as a
     * NEGATIVE quadword whose magnitude is the interval in 100ns units
     * (the same negative-is-delta convention sys$setimr uses).  Detect it
     * by the absence of the "dd-MMM-yyyy" date part: a delta string has a
     * ':' but no '-' introducing a month.
     */
    if (strchr(buf, ':') != NULL && strstr(buf, "-") == NULL) {
        int d_days = 0, d_h = 0, d_m = 0, d_s = 0, d_cc = 0;
        int n = sscanf(buf, "%d %d:%d:%d.%d",
                       &d_days, &d_h, &d_m, &d_s, &d_cc);
        if (n < 4) {
            /* No leading day count: "hh:mm:ss[.cc]" */
            d_days = 0;
            n = sscanf(buf, "%d:%d:%d.%d", &d_h, &d_m, &d_s, &d_cc);
            if (n < 3) return SS$_BADPARAM;
        }
        if (d_days < 0 || d_h < 0 || d_m < 0 || d_s < 0 || d_cc < 0)
            return SS$_BADPARAM;

        uint64_t ticks =
            ((uint64_t)d_days * 86400ULL + (uint64_t)d_h * 3600ULL +
             (uint64_t)d_m * 60ULL + (uint64_t)d_s) * 10000000ULL +
            (uint64_t)d_cc * 100000ULL;
        *q = (uint64_t)(-(int64_t)ticks);   /* delta: stored negative */
        return SS$_NORMAL;
    }

    if (sscanf(buf, "%d-%3s-%d %d:%d:%d.%d",
               &day, mon, &year, &hour, &min, &sec, &hun) < 3) {
        return SS$_BADPARAM;
    }

    int month = -1;
    for (int i = 0; i < 12; i++) {
        if (strncasecmp(mon, months[i], 3) == 0) {
            month = i;
            break;
        }
    }
    if (month < 0) return SS$_BADPARAM;

    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    tm_val.tm_year = year - 1900;
    tm_val.tm_mon = month;
    tm_val.tm_mday = day;
    tm_val.tm_hour = hour;
    tm_val.tm_min = min;
    tm_val.tm_sec = sec;

    time_t t = ovmx_timegm(&tm_val);
    struct timespec ts = { .tv_sec = t, .tv_nsec = hun * 10000000L };
    *q = unix_to_vms_time(&ts);

    return SS$_NORMAL;
}

/* ---- Timer management ---- */

#if OVMX_HOST_POSIX_TIMERS
#define MAX_TIMERS 32

struct timer_entry {
    uint32_t    reqidt;          /* Request identification */
    uint32_t    efn;             /* Event flag to set on expiry */
    void      (*astadr)(uint32_t); /* AST routine to call */
    uint32_t    astprm;          /* AST parameter */
    timer_t     timerid;         /* POSIX timer ID */
    int         active;          /* Entry in use */
};

static struct timer_entry timer_table[MAX_TIMERS];
static pthread_mutex_t timer_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Signal handler for POSIX timer expiry */
static void timer_signal_handler(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)uc;

    struct timer_entry *te = (struct timer_entry *)si->si_value.sival_ptr;
    if (!te || !te->active) return;

    /* Set the event flag */
    if ((te->efn & 0xFFu) < 128) {
        sys$setef(te->efn);
    }

    /* Call the AST routine */
    if (te->astadr) {
        te->astadr(te->astprm);
    }

    te->active = 0;
}

static pthread_once_t timer_once = PTHREAD_ONCE_INIT;

static void init_timer_signals_once(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sa.sa_sigaction = timer_signal_handler;
    sigaction(SIGRTMIN, &sa, NULL);
}

static void init_timer_signals(void) {
    pthread_once(&timer_once, init_timer_signals_once);
}

#endif /* OVMX_HOST_POSIX_TIMERS */

#if !OVMX_HOST_POSIX_TIMERS
/*
 * The OpenVMS-calling-standard timer queue (rd vms-a06).
 *
 * Every request is a slot with an absolute expiry; ITIMER_REAL is armed for
 * the earliest one. On SIGALRM each expired slot sets its event flag (the
 * executive's, through $SETEF) and calls its AST routine with the request id,
 * then the timer is re-armed for the next. Slots change with SIGALRM blocked.
 *
 * Entering the handler. Linux/Alpha enters a signal handler with PC = R27 =
 * the handler address, R16..R18 = signo, siginfo, ucontext, and R26 = the
 * restorer. A procedure of the OpenVMS calling standard is named by its
 * procedure descriptor and expects R27 = that descriptor and R25 = the
 * argument information. So the address given to the kernel is a 4-instruction
 * thunk followed by the descriptor's address:
 *     ldq $27,16($27)   R27 = the handler's procedure descriptor
 *     ldq $1,8($27)     its entry address (PDSC$Q_ENTRY)
 *     lda $25,3($31)    three integer arguments
 *     jmp $31,($1)
 * and the restorer is   mov $30,$16 ; lda $0,351($31) ; callsys
 * (rt_sigreturn). Encodings checked against alpha-dec-vms-as. The handler is
 * installed with the raw rt_sigaction call so the restorer reaches the kernel
 * (Linux/Alpha passes it as rt_sigaction's fifth argument).
 */
#include <sys/mman.h>

#define VT_MAX 32
#define VT_SIGALRM 14                  /* Linux/Alpha */
#define VT_SA_SIGINFO 0x40u
#define VT_SA_RESTART 0x2u
#define VT_NR_RT_SIGACTION 352
#define VT_NR_RT_SIGPROCMASK 353
#define VT_NR_SETITIMER 362
#define VT_SIG_BLOCK 1                 /* Linux/Alpha SIG_BLOCK */
#define VT_SIG_SETMASK 3               /* Linux/Alpha SIG_SETMASK */

static struct vt_slot {
    int       active;
    uint32_t  reqidt;
    uint32_t  efn;
    void    (*astadr)(uint32_t);
    uint64_t  due_ns;                  /* CLOCK_REALTIME */
} vt[VT_MAX];
static int vt_installed;

static long long vt_syscall(long long n, long long a1, long long a2, long long a3,
                            long long a4, long long a5)
{
    register long long r0  __asm__("$0")  = n;
    register long long r16 __asm__("$16") = a1;
    register long long r17 __asm__("$17") = a2;
    register long long r18 __asm__("$18") = a3;
    register long long r19 __asm__("$19") = a4;
    register long long r20 __asm__("$20") = a5;
    __asm__ __volatile__("callsys"
                         : "+r"(r0), "+r"(r19)
                         : "r"(r16), "r"(r17), "r"(r18), "r"(r20)
                         : "$1", "$2", "$3", "$4", "$5", "$6", "$7", "$8", "$21",
                           "$22", "$23", "$24", "$25", "$27", "$28", "memory");
    return r19 ? -r0 : r0;
}

static uint64_t vt_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t vt_block(void)
{
    uint64_t set = 1ULL << (VT_SIGALRM - 1), old = 0;
    vt_syscall(VT_NR_RT_SIGPROCMASK, VT_SIG_BLOCK, (long long)(uintptr_t)&set,
               (long long)(uintptr_t)&old, 8, 0);
    return old;
}

static void vt_restore(uint64_t old)
{
    vt_syscall(VT_NR_RT_SIGPROCMASK, VT_SIG_SETMASK, (long long)(uintptr_t)&old, 0, 8, 0);
}

/* Arm ITIMER_REAL for the earliest pending slot, or disarm it. */
static void vt_arm(void)
{
    uint64_t due = 0, now = vt_now();
    for (int i = 0; i < VT_MAX; i++)
        if (vt[i].active && (!due || vt[i].due_ns < due))
            due = vt[i].due_ns;
    long long it[4] = { 0, 0, 0, 0 };  /* it_interval {sec,usec}, it_value {sec,usec} */
    if (due) {
        uint64_t d = due > now ? due - now : 0;
        uint64_t us = d / 1000ULL;
        if (us == 0)
            us = 1;
        it[2] = (long long)(us / 1000000ULL);
        it[3] = (long long)(us % 1000000ULL);
    }
    vt_syscall(VT_NR_SETITIMER, 0 /* ITIMER_REAL */, (long long)(uintptr_t)it, 0, 0, 0);
}

static void vt_alarm(int sig, void *si, void *uc)
{
    (void)sig; (void)si; (void)uc;
    uint64_t now = vt_now();
    for (int i = 0; i < VT_MAX; i++) {
        if (!vt[i].active || vt[i].due_ns > now)
            continue;
        struct vt_slot s = vt[i];
        vt[i].active = 0;
        if ((s.efn & 0xFFu) != EFN$C_ENF)
            (void)sys$setef(s.efn);
        if (s.astadr)
            s.astadr(s.reqidt);
    }
    vt_arm();
}

static int vt_install(void)
{
    if (vt_installed)
        return 1;
    uint32_t *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED)
        return 0;
    code[0] = 0xA77B0010u;            /* ldq $27,16($27) */
    code[1] = 0xA43B0008u;            /* ldq $1,8($27)   */
    code[2] = 0x233F0003u;            /* lda $25,3($31)  */
    code[3] = 0x6BE10000u;            /* jmp $31,($1)    */
    void (*h)(int, void *, void *) = vt_alarm;
    memcpy(&code[4], &h, sizeof h);   /* the procedure descriptor's address */
    code[8]  = 0x47FE0410u;           /* mov $30,$16     */
    code[9]  = 0x201F015Fu;           /* lda $0,351($31) */
    code[10] = 0x00000083u;           /* callsys         */
    __asm__ __volatile__("call_pal 0x86" ::: "memory");   /* imb */
    struct { unsigned long long handler, flags, mask; } ka = {
        (unsigned long long)(uintptr_t)code, VT_SA_SIGINFO | VT_SA_RESTART, 0
    };
    if (vt_syscall(VT_NR_RT_SIGACTION, VT_SIGALRM, (long long)(uintptr_t)&ka, 0, 8,
                   (long long)(uintptr_t)&code[8]) < 0)
        return 0;
    vt_installed = 1;
    return 1;
}
#endif /* !OVMX_HOST_POSIX_TIMERS */

/*
 * sys$setimr - Set timer request.
 *
 * Schedules a timer that, when it expires, sets the event flag efn
 * and optionally calls the AST routine astadr.
 *
 * VMS delta times are negative 64-bit values (negative = relative,
 * positive = absolute). We handle both cases.
 */
uint32_t sys$setimr(uint32_t efn, const uint64_t *daytim,
                    void (*astadr)(uint32_t), uint32_t reqidt,
                    uint32_t flags) {
    (void)flags;

    if (!daytim) return SS$_BADPARAM;
#if !OVMX_HOST_POSIX_TIMERS
    {
        if ((efn & 0xFFu) != EFN$C_ENF) {
            uint32_t cst = sys$clref(efn);
            if (!(cst & 1))
                return cst;
        }
        int64_t t = (int64_t)*daytim;
        uint64_t due;
        if (t < 0) {
            due = vt_now() + (uint64_t)(-t) * 100ULL;
        } else {
            struct timespec abs_ts;
            if (vms_to_unix_time((uint64_t)t, &abs_ts) < 0)
                return SS$_BADPARAM;
            due = (uint64_t)abs_ts.tv_sec * 1000000000ULL + (uint64_t)abs_ts.tv_nsec;
        }
        if (!vt_install())
            return SS$_INSFMEM;
        uint64_t old = vt_block();
        int slot = -1;
        for (int i = 0; i < VT_MAX; i++)
            if (!vt[i].active) { slot = i; break; }
        if (slot < 0) {
            vt_restore(old);
            return SS$_EXQUOTA;
        }
        vt[slot].reqidt = reqidt;
        vt[slot].efn = efn;
        vt[slot].astadr = astadr;
        vt[slot].due_ns = due;
        vt[slot].active = 1;
        vt_arm();
        vt_restore(old);
        return SS$_NORMAL;
    }
#else

    /* The flag is CLEARED when the request is queued, and an efn that is not
     * one of this process's flags fails the request (SS$_UNASEFC for an
     * unassociated common cluster, SS$_ILLEFC) -- observed on OpenVMS VAX V7.3
     * and Alpha V8.4 by the semantic oracle (docs/oracle/semantics/ef/, cases
     * EF.SETIMR.*, rd vms-d08). The executive answers through $CLREF. Without
     * the clear, a $WAITFR on the timer's flag returns at once if the flag was
     * already set, before the timer has fired. EFN$C_ENF names no flag. */
    if ((efn & 0xFFu) != EFN$C_ENF) {
        uint32_t cst = sys$clref(efn);
        if (!(cst & 1))
            return cst;
    }

    init_timer_signals();

    pthread_mutex_lock(&timer_mutex);

    /* Find a free timer slot */
    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timer_table[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&timer_mutex);
        return SS$_EXQUOTA;
    }

    struct timer_entry *te = &timer_table[slot];
    te->reqidt = reqidt;
    te->efn = efn;
    te->astadr = astadr;
    te->astprm = reqidt;
    te->active = 1;

    /* Create a POSIX timer */
    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGRTMIN;
    sev.sigev_value.sival_ptr = te;

    if (timer_create(CLOCK_REALTIME, &sev, &te->timerid) < 0) {
        te->active = 0;
        pthread_mutex_unlock(&timer_mutex);
        return SS$_INSFMEM;
    }

    /* Convert VMS time to timespec for timer_settime */
    struct itimerspec its;
    memset(&its, 0, sizeof(its));

    int64_t signed_time = (int64_t)*daytim;
    if (signed_time < 0) {
        /* Delta time (relative) - VMS convention: negative = delta */
        uint64_t ticks = (uint64_t)(-signed_time);
        its.it_value.tv_sec = (time_t)(ticks / 10000000ULL);
        its.it_value.tv_nsec = (long)((ticks % 10000000ULL) * 100);
        if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0) {
            its.it_value.tv_nsec = 1;  /* Must be nonzero */
        }
    } else {
        /* Absolute time - convert from VMS to Unix */
        struct timespec abs_ts;
        if (vms_to_unix_time((uint64_t)signed_time, &abs_ts) < 0) {
            te->active = 0;
            pthread_mutex_unlock(&timer_mutex);
            return SS$_BADPARAM;
        }
        its.it_value = abs_ts;
    }

    int timer_flags = (signed_time >= 0) ? TIMER_ABSTIME : 0;
    if (timer_settime(te->timerid, timer_flags, &its, NULL) < 0) {
        timer_delete(te->timerid);
        te->active = 0;
        pthread_mutex_unlock(&timer_mutex);
        return SS$_BADPARAM;
    }

    pthread_mutex_unlock(&timer_mutex);
    return SS$_NORMAL;
#endif /* OVMX_HOST_POSIX_TIMERS */
}

/*
 * sys$cantim - Cancel timer request.
 *
 * Cancels all timer requests matching the given reqidt.
 * If reqidt is 0, cancels all timers.
 */
uint32_t sys$cantim(uint32_t reqidt, uint32_t acmode) {
    (void)acmode;

#if OVMX_HOST_POSIX_TIMERS
    pthread_mutex_lock(&timer_mutex);
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timer_table[i].active &&
            (reqidt == 0 || timer_table[i].reqidt == reqidt)) {
            timer_delete(timer_table[i].timerid);
            timer_table[i].active = 0;
        }
    }
    pthread_mutex_unlock(&timer_mutex);
#else
    {
        uint64_t old = vt_block();
        for (int i = 0; i < VT_MAX; i++)
            if (vt[i].active && (reqidt == 0 || vt[i].reqidt == reqidt))
                vt[i].active = 0;
        if (vt_installed)
            vt_arm();
        vt_restore(old);
    }
#endif

    return SS$_NORMAL;
}

/*
 * sys$gettim_prec - Get the current time at the best precision available.
 * The host's CLOCK_REALTIME is read at nanosecond resolution and reported in
 * VMS 100ns ticks, so the full-precision answer is the only one given.
 */
uint32_t sys$gettim_prec(uint64_t *timadr)
{
    return sys$gettim(timadr);
}

/*
 * $SCHDWK / $CANWAK -- schedule a wakeup of a process, cancel scheduled wakeups.
 *
 * Built on the process-local timer table of sys$setimr: each request occupies a
 * slot recording the target, and the timer's AST issues $WAKE for it. reqidt
 * values carry WAKE_REQID_BASE so $CANWAK cancels only wakeups, never a
 * caller's own timers.
 */
#define WAKE_REQID_BASE 0x57414B00u      /* 'WAK' + slot */
#define WAKE_SLOTS      16

static struct {
    int      in_use;
    int      has_pid;
    uint32_t pid;
} wake_slot[WAKE_SLOTS];

static void wake_ast(uint32_t reqidt)
{
    unsigned slot = reqidt & 0xFFu;
    if (slot >= WAKE_SLOTS || !wake_slot[slot].in_use)
        return;
    uint32_t pid = wake_slot[slot].pid;
    int has = wake_slot[slot].has_pid;
    wake_slot[slot].in_use = 0;
    (void)sys$wake(has ? &pid : NULL, NULL);
}

uint32_t sys$schdwk(const uint32_t *pidadr, const struct dsc$descriptor_s *prcnam,
                    const uint64_t *daytim, const uint64_t *reptim)
{
    if (!daytim)
        return SS$_BADPARAM;
    if (reptim)
        return SS$_BADPARAM;            /* a repeating wakeup is not supported */
    if (prcnam && prcnam->dsc$a_pointer && prcnam->dsc$w_length > 0)
        return SS$_BADPARAM;            /* waking by process name is not supported */

    unsigned slot;
    for (slot = 0; slot < WAKE_SLOTS; slot++)
        if (!wake_slot[slot].in_use)
            break;
    if (slot == WAKE_SLOTS)
        return SS$_EXQUOTA;
    wake_slot[slot].in_use = 1;
    wake_slot[slot].has_pid = (pidadr && *pidadr != 0);
    wake_slot[slot].pid = wake_slot[slot].has_pid ? *pidadr : 0;

    /* EFN$C_ENF: a scheduled wakeup touches no event flag ($SCHDWK has no efn
     * argument; EF 0 belongs to the caller, and $SETIMR clears and sets the
     * flag it is given). */
    uint32_t st = sys$setimr(EFN$C_ENF, daytim, wake_ast, WAKE_REQID_BASE | slot, 0);
    if (!(st & 1))
        wake_slot[slot].in_use = 0;
    return st;
}

uint32_t sys$canwak(const uint32_t *pidadr, const struct dsc$descriptor_s *prcnam)
{
    if (prcnam && prcnam->dsc$a_pointer && prcnam->dsc$w_length > 0)
        return SS$_BADPARAM;
    int has = (pidadr && *pidadr != 0);
    for (unsigned slot = 0; slot < WAKE_SLOTS; slot++) {
        if (!wake_slot[slot].in_use)
            continue;
        if (has != wake_slot[slot].has_pid || (has && wake_slot[slot].pid != *pidadr))
            continue;
        (void)sys$cantim(WAKE_REQID_BASE | slot, 0);
        wake_slot[slot].in_use = 0;
    }
    return SS$_NORMAL;
}


/* =========================================================================
 * UTC services -- $GETUTC / $BINUTC / $NUMUTC / $ASCUTC / $TIMCON
 *
 * The UTC time structure is the 16 bytes of $UTCDEF (OpenVMS Alpha V8.4, LIB.MLB):
 *   +0  UTC$Q_ABSTIME  8   100 ns ticks since 15-OCT-1582 (the VMS system time + the
 *                          1582 -> 1858 offset)
 *   +8  UTC$A_INACCUR  6   inaccuracy; all ones = unspecified
 *   +14 UTC$W_TDFWRD   2   TDF in minutes east of UTC (12 bits, two's complement,
 *                          mask 0xFFF) and the structure VERSION (4 bits, <15:12>)
 *
 * Observed on the lab Alpha V8.4 node (MACRO-32 probes, tools/lab-alpha/probes,
 * SYS$TIMEZONE_DIFFERENTIAL = 0): $BINUTC("29-FEB-2000 12:23:45.67") ->
 * 11081A60 01D3EEA3 FFFFFFFF 1000FFFF; $NUMUTC of that -> 2000 2 29 12 23 45 67;
 * $ASCUTC -> "29-FEB-2000 12:23:45.67", cvtflg bit 0 -> "12:23:45.67" (time only);
 * $TIMCON(utc, 0) -> the system quadword 009E6638 49721A60, $TIMCON(quad, 1) the same
 * UTC back; $GETUTC returns the same structure for "now" (inaccuracy ffff..., TDF
 * word 0x1000). The system TDF comes from the SYS$TIMEZONE_DIFFERENTIAL logical
 * (seconds east of UTC); non-zero TDFs were NOT observable on the lab (the logical
 * does not move the kernel TDF there), so the TDF arithmetic below follows the
 * documented model (local = UTC + TDF) and is unverified against an oracle.
 * ========================================================================= */
#define UTC_EPOCH_OFFSET 0x0135886AC7960000ULL   /* 100 ns: 15-OCT-1582 -> 17-NOV-1858 = 100840 days */
#define UTC_TICKS_PER_MIN 600000000LL
#define UTC_VERSION 1

static void utc_put(uint8_t *u, uint64_t abstime, int tdf_min)
{
    for (int i = 0; i < 8; i++) u[i] = (uint8_t)(abstime >> (8 * i));
    memset(u + 8, 0xFF, 6);                          /* inaccuracy: unspecified */
    uint16_t w = (uint16_t)(((unsigned)UTC_VERSION << 12) | ((unsigned)tdf_min & 0xFFF));
    u[14] = (uint8_t)w; u[15] = (uint8_t)(w >> 8);
}

static uint64_t utc_abs(const uint8_t *u)
{
    uint64_t t = 0;
    for (int i = 0; i < 8; i++) t |= (uint64_t)u[i] << (8 * i);
    return t;
}

/* TDF word -> signed minutes (12-bit two's complement) */
static int utc_tdf(const uint8_t *u)
{
    int v = (u[14] | (u[15] << 8)) & 0xFFF;
    return v >= 0x800 ? v - 0x1000 : v;
}

/* The system TDF in minutes east of UTC: the SYS$TIMEZONE_DIFFERENTIAL logical (seconds). */
static int utc_system_tdf(void)
{
    static const struct dsc$descriptor_s tab = { 12, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"LNM$FILE_DEV" };
    static const struct dsc$descriptor_s nam = { 25, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"SYS$TIMEZONE_DIFFERENTIAL" };
    char val[32];
    uint16_t len = 0;
    struct item_list_3 il[2];
    memset(val, 0, sizeof val);
    il[0].buflen = sizeof(val) - 1; il[0].item_code = 2 /* LNM$_STRING */;
    il[0].bufaddr = val; il[0].retlen = &len;
    il[1].buflen = 0; il[1].item_code = 0; il[1].bufaddr = NULL; il[1].retlen = NULL;
    if (!(sys$trnlnm(NULL, &tab, &nam, NULL, il) & 1) || len == 0)
        return 0;
    long secs = strtol(val, NULL, 10);
    long m = secs / 60;
    return (m < -2047 || m > 2047) ? 0 : (int)m;
}

uint32_t (sys$getutc)(void *utcadr)
{
    if (!utcadr) return SS$_BADPARAM;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    utc_put(utcadr, unix_to_vms_time(&ts) + UTC_EPOCH_OFFSET, utc_system_tdf());
    return SS$_NORMAL;
}

/* $TIMCON(timadr, utcadr, cvtflg): cvtflg 0 = UTC -> the system-time quadword (local, at
 * the structure's TDF); 1 = quadword -> UTC (at the system TDF). */
uint32_t (sys$timcon)(uint64_t *timadr, void *utcadr, uint32_t cvtflg)
{
    if (!timadr || !utcadr) return SS$_ACCVIO;
    if (cvtflg == 0) {
        int tdf = utc_tdf(utcadr);
        *timadr = (uint64_t)((int64_t)(utc_abs(utcadr) - UTC_EPOCH_OFFSET)
                             + (int64_t)tdf * UTC_TICKS_PER_MIN);
        return SS$_NORMAL;
    }
    if (cvtflg == 1) {
        int tdf = utc_system_tdf();
        utc_put(utcadr, (uint64_t)((int64_t)*timadr - (int64_t)tdf * UTC_TICKS_PER_MIN)
                        + UTC_EPOCH_OFFSET, tdf);
        return SS$_NORMAL;
    }
    return SS$_BADPARAM;
}

uint32_t (sys$binutc)(const struct dsc$descriptor_s *timbuf, void *utcadr)
{
    struct _generic_64 q;
    uint64_t now;
    if (!utcadr) return SS$_ACCVIO;
    if (!timbuf || !timbuf->dsc$a_pointer || timbuf->dsc$w_length == 0) {
        sys$gettim(&now);                 /* no string: the current time */
        uint32_t st = SS$_NORMAL;
        (void)st;
        return sys$timcon(&now, utcadr, 1);
    }
    uint32_t st = sys$bintim(timbuf, &q);
    if (!(st & 1)) return st;
    memcpy(&now, &q, sizeof now);
    return sys$timcon(&now, utcadr, 1);
}

uint32_t (sys$numutc)(uint16_t *timbuf, const void *utcadr)
{
    uint64_t q;
    if (!timbuf || !utcadr) return SS$_ACCVIO;
    sys$timcon(&q, (void *)(uintptr_t)utcadr, 0);
    return sys$numtim(timbuf, &q);
}

uint32_t (sys$ascutc)(uint16_t *timlen, struct dsc$descriptor_s *timbuf,
                      const void *utcadr, uint32_t cvtflg)
{
    uint64_t q;
    if (!utcadr) return SS$_ACCVIO;
    sys$timcon(&q, (void *)(uintptr_t)utcadr, 0);
    return sys$asctim(timlen, timbuf, &q, cvtflg);
}
