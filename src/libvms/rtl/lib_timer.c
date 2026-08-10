/*
 * lib_timer.c - VMS LIB$ timing / statistics routines
 *
 * Implements the LIB$ "timer" facility:
 *
 *   LIB$INIT_TIMER  - snapshot the caller's resource usage into a context
 *   LIB$SHOW_TIMER  - format and output the accumulated statistics
 *   LIB$STAT_TIMER  - return one statistic (elapsed/CPU/BUFIO/DIRIO/faults)
 *   LIB$FREE_TIMER  - release a context allocated by LIB$INIT_TIMER
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual — LIB$INIT_TIMER,
 * LIB$SHOW_TIMER, LIB$STAT_TIMER, LIB$FREE_TIMER. Those routines report
 * five statistics keyed by a "code" argument:
 *
 *   code 1 = elapsed real (wall-clock) time, returned as a quadword
 *            delta time
 *   code 2 = elapsed CPU time, in 10-millisecond units (longword)
 *   code 3 = number of buffered I/O operations (longword)
 *   code 4 = number of direct I/O operations (longword)
 *   code 5 = number of page faults (longword)
 *
 * VMS allocates the statistics block through LIB$GET_VM and stores its
 * address in the caller's handle longword. The Eight-Cubed corpus (and
 * every other 32-bit VMS caller) declares that handle as a 32-bit
 * `unsigned int`, so a real 64-bit heap pointer will NOT fit there. We
 * therefore keep the contexts in a small fixed table and store a 1-based
 * slot index in the handle — 32-bit-safe, and the handle stays opaque to
 * the caller exactly as on VMS.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <sys/resource.h>
#include <pthread.h>
#include "ssdef.h"
#include "descrip.h"
#include "libwaitdef.h"
#include "lib$routines.h"

#define MAX_TIMERS 128

struct timer_ctx {
    int              active;
    struct timespec  wall;   /* CLOCK_MONOTONIC snapshot at init */
    struct rusage    ru;     /* resource usage snapshot at init  */
};

static struct timer_ctx timers[MAX_TIMERS];
static struct timer_ctx default_timer;   /* used when handle == NULL */
static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;

/* Snapshot the current wall clock + resource usage into a context. */
static void timer_snapshot(struct timer_ctx *t)
{
    clock_gettime(CLOCK_MONOTONIC, &t->wall);
    getrusage(RUSAGE_SELF, &t->ru);
    t->active = 1;
}

/* Resolve a handle to its context, or NULL if invalid. */
static struct timer_ctx *timer_resolve(uint32_t *handle)
{
    if (!handle)
        return &default_timer;
    uint32_t slot = *handle;
    if (slot == 0 || slot > MAX_TIMERS)
        return NULL;
    if (!timers[slot - 1].active)
        return NULL;
    return &timers[slot - 1];
}

/*
 * lib$init_timer - Snapshot resource usage into a (possibly new) context.
 *
 * If handle is NULL, the process-wide default context is used. If handle
 * points to 0, a new context slot is allocated and its 1-based index is
 * written back through handle. If handle points to an existing slot, that
 * slot is re-snapshot (VMS allows re-initializing a handle).
 */
uint32_t lib$init_timer(uint32_t *handle)
{
    if (!handle) {
        pthread_mutex_lock(&timer_lock);
        timer_snapshot(&default_timer);
        pthread_mutex_unlock(&timer_lock);
        return SS$_NORMAL;
    }

    pthread_mutex_lock(&timer_lock);

    uint32_t slot = *handle;
    if (slot != 0 && slot <= MAX_TIMERS && timers[slot - 1].active) {
        /* Re-initialize an existing context. */
        timer_snapshot(&timers[slot - 1]);
        pthread_mutex_unlock(&timer_lock);
        return SS$_NORMAL;
    }

    /* Allocate a new slot. */
    uint32_t i;
    for (i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].active)
            break;
    }
    if (i >= MAX_TIMERS) {
        pthread_mutex_unlock(&timer_lock);
        return SS$_INSFMEM;
    }

    timer_snapshot(&timers[i]);
    *handle = i + 1;

    pthread_mutex_unlock(&timer_lock);
    return SS$_NORMAL;
}

/* Compute elapsed wall time in 100ns ticks since the snapshot. */
static uint64_t elapsed_ticks(const struct timer_ctx *t)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t sec  = (int64_t)now.tv_sec  - (int64_t)t->wall.tv_sec;
    int64_t nsec = (int64_t)now.tv_nsec - (int64_t)t->wall.tv_nsec;
    int64_t total_ns = sec * 1000000000LL + nsec;
    if (total_ns < 0)
        total_ns = 0;
    return (uint64_t)total_ns / 100ULL;   /* ns -> 100ns ticks */
}

/* Sum a struct timeval into 10-millisecond CPU units. */
static uint64_t tv_to_cpu_units(const struct timeval *tv)
{
    return (uint64_t)tv->tv_sec * 100ULL + (uint64_t)tv->tv_usec / 10000ULL;
}

/*
 * lib$stat_timer - Return a single accumulated statistic.
 *
 * code selects which statistic; value receives it. For code 1 (elapsed
 * real time) the value is a quadword delta time (stored negative, the
 * OVMX delta-time convention), suitable for LIB$SYS_ASCTIM. For codes
 * 2-5 the value is a longword.
 */
uint32_t lib$stat_timer(const int32_t *code, void *value, uint32_t *handle)
{
    if (!code || !value)
        return SS$_BADPARAM;

    struct timer_ctx *t = timer_resolve(handle);
    if (!t)
        return SS$_BADPARAM;

    struct rusage now;
    getrusage(RUSAGE_SELF, &now);

    switch (*code) {
    case 1: {
        /* Elapsed real time as a quadword delta (negative magnitude). */
        uint64_t ticks = elapsed_ticks(t);
        *(uint64_t *)value = (uint64_t)(-(int64_t)ticks);
        break;
    }
    case 2: {
        /* Elapsed CPU time (user+system) in 10ms units. */
        uint64_t cpu = (tv_to_cpu_units(&now.ru_utime) +
                        tv_to_cpu_units(&now.ru_stime)) -
                       (tv_to_cpu_units(&t->ru.ru_utime) +
                        tv_to_cpu_units(&t->ru.ru_stime));
        *(uint32_t *)value = (uint32_t)cpu;
        break;
    }
    case 3: {
        /* Buffered I/O count (message send/receive). */
        long bio = (now.ru_msgsnd + now.ru_msgrcv) -
                   (t->ru.ru_msgsnd + t->ru.ru_msgrcv);
        *(uint32_t *)value = (uint32_t)(bio < 0 ? 0 : bio);
        break;
    }
    case 4: {
        /* Direct I/O count (block input/output). */
        long dio = (now.ru_inblock + now.ru_oublock) -
                   (t->ru.ru_inblock + t->ru.ru_oublock);
        *(uint32_t *)value = (uint32_t)(dio < 0 ? 0 : dio);
        break;
    }
    case 5: {
        /* Page faults (major + minor). */
        long flt = (now.ru_majflt + now.ru_minflt) -
                   (t->ru.ru_majflt + t->ru.ru_minflt);
        *(uint32_t *)value = (uint32_t)(flt < 0 ? 0 : flt);
        break;
    }
    default:
        return LIB$_INVARG;
    }

    return SS$_NORMAL;
}

/*
 * lib$show_timer - Format the accumulated statistics and output them.
 *
 * VMS supports an optional code, action routine and user argument. When
 * no action routine is supplied the formatted line is written to
 * SYS$OUTPUT (stdout here). This produces the customary summary line:
 *   ELAPSED: dddd hh:mm:ss.cc  CPU: hh:mm:ss.cc  BUFIO: n  DIRIO: n  FAULTS: n
 */
uint32_t lib$show_timer(uint32_t *handle, ...)
{
    struct timer_ctx *t = timer_resolve(handle);
    if (!t)
        return SS$_BADPARAM;

    uint64_t ticks = elapsed_ticks(t);           /* 100ns units */
    uint64_t hun = ticks / 100000ULL;            /* hundredths */
    unsigned cc = (unsigned)(hun % 100); hun /= 100;
    unsigned ss = (unsigned)(hun % 60);  hun /= 60;
    unsigned mm = (unsigned)(hun % 60);  hun /= 60;
    unsigned hh = (unsigned)(hun % 24);  hun /= 24;
    unsigned dd = (unsigned)hun;

    struct rusage now;
    getrusage(RUSAGE_SELF, &now);
    uint64_t cpu = (tv_to_cpu_units(&now.ru_utime) +
                    tv_to_cpu_units(&now.ru_stime)) -
                   (tv_to_cpu_units(&t->ru.ru_utime) +
                    tv_to_cpu_units(&t->ru.ru_stime));
    unsigned ccc = (unsigned)(cpu % 100); cpu /= 100;
    unsigned css = (unsigned)(cpu % 60);  cpu /= 60;
    unsigned cmm = (unsigned)(cpu % 60);  cpu /= 60;
    unsigned chh = (unsigned)cpu;

    long dio = (now.ru_inblock + now.ru_oublock) -
               (t->ru.ru_inblock + t->ru.ru_oublock);
    long bio = (now.ru_msgsnd + now.ru_msgrcv) -
               (t->ru.ru_msgsnd + t->ru.ru_msgrcv);
    long flt = (now.ru_majflt + now.ru_minflt) -
               (t->ru.ru_majflt + t->ru.ru_minflt);

    printf("ELAPSED: %u %02u:%02u:%02u.%02u  CPU: %02u:%02u:%02u.%02u  "
           "BUFIO: %ld  DIRIO: %ld  FAULTS: %ld\n",
           dd, hh, mm, ss, cc, chh, cmm, css, ccc,
           bio < 0 ? 0 : bio, dio < 0 ? 0 : dio, flt < 0 ? 0 : flt);
    fflush(stdout);

    return SS$_NORMAL;
}

/*
 * lib$free_timer - Release a context allocated by lib$init_timer.
 */
uint32_t lib$free_timer(uint32_t *handle)
{
    if (!handle) {
        pthread_mutex_lock(&timer_lock);
        default_timer.active = 0;
        pthread_mutex_unlock(&timer_lock);
        return SS$_NORMAL;
    }

    uint32_t slot = *handle;
    if (slot == 0 || slot > MAX_TIMERS)
        return SS$_BADPARAM;

    pthread_mutex_lock(&timer_lock);
    timers[slot - 1].active = 0;
    pthread_mutex_unlock(&timer_lock);

    *handle = 0;
    return SS$_NORMAL;
}

/* ================================================================
 * LIB$WAIT - suspend the caller for a real-time interval
 *
 * lib$wait(seconds, [flags], [float-type]) delays the calling thread
 * for the number of seconds given by the floating value at `seconds`.
 * The float-type argument names the storage format of that value:
 *
 *   LIB$K_VAX_F / VAX_D / VAX_G  - VAX F/D/G_floating
 *   LIB$K_IEEE_S / IEEE_T        - IEEE single / double
 *
 * When float-type is omitted the value is taken to be IEEE single, the
 * native single-precision format on this (x86-64 / IEEE) platform.  The
 * VAX formats are decoded from their documented bit layouts so a caller
 * that really holds VAX-format data is honored, even though the OVMX C
 * compiler emits IEEE floats.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual - LIB$WAIT; the VAX
 * floating formats are per the public "VAX Floating-Point" definitions.
 * ================================================================ */

/* Reassemble a VAX float's 16-bit words (stored low-address-first) into
 * the natural big-endian-field ordering used by the bit-field formulas. */
static double vax_f_to_double(const void *p) {
    uint32_t raw;
    memcpy(&raw, p, 4);
    uint32_t w0 = raw & 0xFFFFu;
    uint32_t w1 = (raw >> 16) & 0xFFFFu;
    uint32_t bits = (w0 << 16) | w1;
    int sign = (bits >> 31) & 1;
    int exp  = (int)((bits >> 23) & 0xFF);
    uint32_t frac = bits & 0x7FFFFFu;
    if (exp == 0) return sign ? -0.0 : 0.0;   /* zero / (reserved) */
    double m = (double)((1u << 23) | frac) / (double)(1u << 24); /* [0.5,1) */
    double val = ldexp(m, exp - 128);
    return sign ? -val : val;
}

static uint64_t vax_reassemble64(const void *p) {
    uint64_t raw;
    memcpy(&raw, p, 8);
    uint64_t w0 = (raw >>  0) & 0xFFFFu;
    uint64_t w1 = (raw >> 16) & 0xFFFFu;
    uint64_t w2 = (raw >> 32) & 0xFFFFu;
    uint64_t w3 = (raw >> 48) & 0xFFFFu;
    return (w0 << 48) | (w1 << 32) | (w2 << 16) | w3;
}

static double vax_d_to_double(const void *p) {
    uint64_t bits = vax_reassemble64(p);
    int sign = (int)((bits >> 63) & 1);
    int exp  = (int)((bits >> 55) & 0xFF);          /* 8-bit, bias 128 */
    uint64_t frac = bits & 0x7FFFFFFFFFFFFFULL;     /* 55 bits */
    if (exp == 0) return sign ? -0.0 : 0.0;
    double m = (double)((1ULL << 55) | frac) / (double)(1ULL << 56); /* [0.5,1) */
    double val = ldexp(m, exp - 128);
    return sign ? -val : val;
}

static double vax_g_to_double(const void *p) {
    uint64_t bits = vax_reassemble64(p);
    int sign = (int)((bits >> 63) & 1);
    int exp  = (int)((bits >> 52) & 0x7FF);         /* 11-bit, bias 1024 */
    uint64_t frac = bits & 0xFFFFFFFFFFFFFULL;      /* 52 bits */
    if (exp == 0) return sign ? -0.0 : 0.0;
    double m = (double)((1ULL << 52) | frac) / (double)(1ULL << 53); /* [0.5,1) */
    double val = ldexp(m, exp - 1024);
    return sign ? -val : val;
}

uint32_t lib$wait(const void *seconds, const uint32_t *flags,
                  const uint32_t *float_type) {
    (void)flags;   /* reserved / control flags (e.g. LIB$K_NOWAKE) */
    if (!seconds)
        return SS$_BADPARAM;

    uint32_t ft = float_type ? *float_type : LIB$K_IEEE_S;
    double secs;
    switch (ft) {
    case LIB$K_IEEE_S: { float f; memcpy(&f, seconds, sizeof f); secs = (double)f; break; }
    case LIB$K_IEEE_T: { double d; memcpy(&d, seconds, sizeof d); secs = d; break; }
    case LIB$K_VAX_F:  secs = vax_f_to_double(seconds); break;
    case LIB$K_VAX_D:  secs = vax_d_to_double(seconds); break;
    case LIB$K_VAX_G:  secs = vax_g_to_double(seconds); break;
    default:           return SS$_BADPARAM;
    }

    if (!(secs >= 0.0))          /* rejects NaN and negatives */
        return SS$_BADPARAM;
    if (secs > 100000.0)         /* documented upper bound */
        secs = 100000.0;

    struct timespec req, rem;
    req.tv_sec  = (time_t)secs;
    req.tv_nsec = (long)((secs - (double)req.tv_sec) * 1e9);
    if (req.tv_nsec < 0) req.tv_nsec = 0;
    if (req.tv_nsec > 999999999L) req.tv_nsec = 999999999L;

    while (nanosleep(&req, &rem) != 0 && errno == EINTR)
        req = rem;             /* resume the remaining interval after an AST/signal */

    return SS$_NORMAL;
}
