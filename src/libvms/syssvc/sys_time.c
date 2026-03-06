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
    (void)cvtflg;
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
    int len = snprintf(buf, sizeof(buf), "%2d-%s-%04d %02d:%02d:%02d.%02d",
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
uint32_t sys$bintim(const struct dsc$descriptor_s *timbuf, uint64_t *timadr) {
    if (!timbuf || !timadr || !timbuf->dsc$a_pointer) return SS$_BADPARAM;

    char buf[64];
    dsc$strncpy(buf, timbuf, sizeof(buf));

    static const char *months[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };

    int day, year, hour = 0, min = 0, sec = 0, hun = 0;
    char mon[4] = {0};

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

    time_t t = timegm(&tm_val);
    struct timespec ts = { .tv_sec = t, .tv_nsec = hun * 10000000L };
    *timadr = unix_to_vms_time(&ts);

    return SS$_NORMAL;
}

/* ---- Timer management ---- */

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
    if (te->efn > 0) {
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
}

/*
 * sys$cantim - Cancel timer request.
 *
 * Cancels all timer requests matching the given reqidt.
 * If reqidt is 0, cancels all timers.
 */
uint32_t sys$cantim(uint32_t reqidt, uint32_t acmode) {
    (void)acmode;

    pthread_mutex_lock(&timer_mutex);
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timer_table[i].active &&
            (reqidt == 0 || timer_table[i].reqidt == reqidt)) {
            timer_delete(timer_table[i].timerid);
            timer_table[i].active = 0;
        }
    }
    pthread_mutex_unlock(&timer_mutex);

    return SS$_NORMAL;
}
