/*
 * ERRNODEF.H - VMS UNIX Signal and Errno Code Definitions
 *
 * OpenVMX compatibility layer - Defines the C$_ constants that map
 * UNIX signal numbers and errno values to VMS-style names, used when
 * interfacing VMS system services with UNIX signal delivery (e.g.,
 * sys$sigprc).
 *
 * Reference: OpenVMS System Services Reference Manual
 *            HP C Run-Time Library Reference Manual for OpenVMS Systems
 */

#ifndef __ERRNODEF_H
#define __ERRNODEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * C$_ — UNIX signal number constants (VMS naming convention)
 *
 * These map to the POSIX signal numbers used by sys$sigprc.
 * ================================================================ */

#define C$_SIGHUP       1   /* Hangup */
#define C$_SIGINT       2   /* Interrupt */
#define C$_SIGQUIT      3   /* Quit */
#define C$_SIGILL       4   /* Illegal instruction */
#define C$_SIGTRAP      5   /* Trace trap */
#define C$_SIGABRT      6   /* Abort */
#define C$_SIGEMT       7   /* EMT instruction */
#define C$_SIGFPE       8   /* Floating point exception */
#define C$_SIGKILL      9   /* Kill */
#define C$_SIGBUS      10   /* Bus error */
#define C$_SIGSEGV     11   /* Segmentation violation */
#define C$_SIGSYS      12   /* Bad system call */
#define C$_SIGPIPE     13   /* Write to pipe with no readers */
#define C$_SIGALRM     14   /* Alarm clock */
#define C$_SIGTERM     15   /* Software termination */
#define C$_SIGURG      16   /* Urgent condition on I/O channel */
#define C$_SIGSTOP     17   /* Sendable stop signal (not from terminal) */
#define C$_SIGTSTP     18   /* Stop signal from terminal */
#define C$_SIGCONT     19   /* Continue after stop */
#define C$_SIGCHLD     20   /* To parent on child stop or exit */
#define C$_SIGTTIN     21   /* Background read attempted from control terminal */
#define C$_SIGTTOU     22   /* Background write attempted to control terminal */
#define C$_SIGIO       23   /* I/O possible, or completed */
#define C$_SIGXCPU     24   /* Cpu time limit exceeded */
#define C$_SIGXFSZ     25   /* File size limit exceeded */
#define C$_SIGVTALRM   26   /* Virtual timer expired */
#define C$_SIGPROF     27   /* Profiling timer expired */
#define C$_SIGWINCH    28   /* Window size change */
#define C$_SIGUSR1     30   /* User-defined signal 1 */
#define C$_SIGUSR2     31   /* User-defined signal 2 */

#ifdef __cplusplus
}
#endif

#endif /* __ERRNODEF_H */
