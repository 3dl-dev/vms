/*
 * ERRCHK.H - Eight-Cubed corpus error-checking macros
 *
 * Compatibility header for the Eight-Cubed example programs.
 * Provides macros that check a VMS status value and take action
 * on failure (bit 0 clear = error).
 *
 * Usage:
 *   errchk_sig(status);    -- signal the condition via lib$signal
 *   errchk_ret(status);    -- return the status to the caller
 *   errchk_abort(status);  -- abort() the process
 *   errchk_stop(status);   -- force image exit via lib$stop
 */

#ifndef __ERRCHK_H
#define __ERRCHK_H

#include <stsdef.h>
#include <lib$routines.h>
#include <stdlib.h>

#define errchk_sig(s) \
    do { if (!$VMS_STATUS_SUCCESS(s)) lib$signal(s); } while (0)

#define errchk_ret(s) \
    do { if (!$VMS_STATUS_SUCCESS(s)) return (s); } while (0)

#define errchk_abort(s) \
    do { if (!$VMS_STATUS_SUCCESS(s)) abort(); } while (0)

#define errchk_stop(s) \
    do { if (!$VMS_STATUS_SUCCESS(s)) lib$stop(s); } while (0)

#endif /* __ERRCHK_H */
