/*
 * ovmx_host_absent.h -- host primitives the OpenVMS C RTL does not have
 * (rd vms-3b3f).
 *
 * A system service built for the OpenVMS Alpha calling standard (the OVMX/Alpha
 * shareables a native VMS image calls) links against DECC$SHR, which exports
 * the DEC C RTL's names only (tools/cross-alpha-vms/musl-arch/
 * decc-crtl-names.txt). POSIX timers, termios, setpriority, sched_yield and the
 * realtime signal range are not among them, so the producer link left each
 * reference at 0 and a service that reached one jumped to address 0. In that
 * build each name below fails honestly instead (-1, errno ENOSYS), and the
 * service takes the failure path it already has for a host that refuses the
 * call. Other builds are unchanged: this header defines nothing there.
 * Include it after the system headers.
 */
#ifndef OVMX_HOST_ABSENT_H
#define OVMX_HOST_ABSENT_H

#if (defined(__alpha) || defined(__alpha__)) && (defined(__VMS) || defined(__vms) || defined(__VMS__))
#define OVMX_HOST_ABSENT 1
#include <errno.h>

static inline int ovmx_host_absent(void)
{
    errno = ENOSYS;
    return -1;
}

#undef timer_create
#undef timer_settime
#undef timer_delete
#undef tcgetattr
#undef tcsetattr
#undef tcflush
#undef setpriority
#undef sched_yield
#define timer_create(c, s, t)       ((void)(c), (void)(s), (void)(t), ovmx_host_absent())
#define timer_settime(t, f, n, o)   ((void)(t), (void)(f), (void)(n), (void)(o), ovmx_host_absent())
#define timer_delete(t)             ((void)(t), ovmx_host_absent())
#define tcgetattr(fd, t)            ((void)(fd), (void)(t), ovmx_host_absent())
#define tcsetattr(fd, a, t)         ((void)(fd), (void)(a), (void)(t), ovmx_host_absent())
#define tcflush(fd, q)              ((void)(fd), (void)(q), ovmx_host_absent())
#define setpriority(w, i, n)        ((void)(w), (void)(i), (void)(n), ovmx_host_absent())
#define sched_yield()               ovmx_host_absent()
#else
#define OVMX_HOST_ABSENT 0
#endif

#endif /* OVMX_HOST_ABSENT_H */
