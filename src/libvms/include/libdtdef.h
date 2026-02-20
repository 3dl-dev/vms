/*
 * LIBDTDEF.H - VMS LIB$ Date/Time Definitions
 *
 * OpenVMX compatibility layer - Defines the LIB$K_ operation codes
 * and LIB$M_ flag constants used by LIB$ date/time conversion
 * routines including:
 *
 *   lib$cvt_from_internal_time  - Convert VMS time to a scalar value
 *   lib$cvtf_from_internal_time - Convert VMS time to floating-point
 *   lib$cvt_to_internal_time    - Convert scalar to VMS quadword time
 *   lib$cvtf_to_internal_time   - Convert floating-point to VMS time
 *   lib$format_date_time        - Format VMS time as a string
 *   lib$get_maximum_date_length - Get max length of formatted date string
 *
 * The operation codes select which time unit to use when converting
 * between VMS internal quadword time and a scalar numeric value.
 *
 * VMS internal time is a signed 64-bit integer representing the
 * number of 100-nanosecond units since 00:00:00.00, November 17, 1858
 * (the Smithsonian base date).  Delta times are stored as negative
 * values (absolute value = duration).
 *
 * Reference: OpenVMS RTL Date/Time (LIB$) Manual
 *            OpenVMS Programming Concepts Manual — Date/Time Support
 */

#ifndef __LIBDTDEF_H
#define __LIBDTDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * LIB$K_ operation codes for lib$cvt_from/to_internal_time
 *
 * These codes select the time unit for scalar conversion.
 * The suffix indicates the scalar type:
 *   _L  = longword integer (int32_t)
 *   _F  = floating-point (float or double, per calling convention)
 *
 * Absolute time operations extract a calendar field from the
 * VMS time value (e.g., the year, month, day-of-year).
 *
 * Delta time operations express a duration as a fraction of a
 * given time unit (e.g., 0.5 = half a day if unit is days).
 * ================================================================ */

/* ----------------------------------------------------------------
 * Delta time operation codes
 * These interpret the time value as a duration (delta time).
 * ---------------------------------------------------------------- */

#define LIB$K_DELTA_SECONDS_L   1   /* Seconds (longword integer) */
#define LIB$K_DELTA_MINUTES_L   2   /* Minutes (longword integer) */
#define LIB$K_DELTA_HOURS_L     3   /* Hours (longword integer) */
#define LIB$K_DELTA_DAYS_L      4   /* Days (longword integer) */
#define LIB$K_DELTA_WEEKS_L     5   /* Weeks (longword integer) */

#define LIB$K_DELTA_SECONDS_F   6   /* Seconds (floating-point) */
#define LIB$K_DELTA_MINUTES_F   7   /* Minutes (floating-point) */
#define LIB$K_DELTA_HOURS_F     8   /* Hours (floating-point) */
#define LIB$K_DELTA_DAYS_F      9   /* Days (floating-point) */
#define LIB$K_DELTA_WEEKS_F     10  /* Weeks (floating-point) */

/* ----------------------------------------------------------------
 * Absolute time operation codes
 * These extract a calendar field from an absolute (wall-clock) time.
 * ---------------------------------------------------------------- */

#define LIB$K_MONTH_OF_YEAR     11  /* Month of year (1-12) */
#define LIB$K_DAY_OF_YEAR       12  /* Day of year (1-366) */
#define LIB$K_HOUR_OF_DAY       13  /* Hour of day (0-23) */
#define LIB$K_MINUTE_OF_HOUR    14  /* Minute of hour (0-59) */
#define LIB$K_SECOND_OF_MINUTE  15  /* Second of minute (0-59) */
#define LIB$K_DAY_OF_MONTH      16  /* Day of month (1-31) */
#define LIB$K_DAY_OF_WEEK       17  /* Day of week (1=Monday, 7=Sunday) */
#define LIB$K_YEAR              18  /* Year (e.g. 2024) */

/* ================================================================
 * LIB$M_ flag constants for lib$format_date_time and
 * lib$get_maximum_date_length
 *
 * These flags are combined (bitwise OR) to select which components
 * are included in the formatted date/time string.
 * ================================================================ */

#define LIB$M_DATE_FIELDS   0x00000001  /* Include date (year, month, day) */
#define LIB$M_TIME_FIELDS   0x00000002  /* Include time (hh:mm:ss.cc) */
#define LIB$M_WEEKDAY       0x00000004  /* Include day-of-week name */
#define LIB$M_RELATIVE      0x00000008  /* Format as delta (relative) time */

/* ================================================================
 * LIB$K_ constants for lib$get_date_format output formats
 *
 * lib$get_date_format returns a format string compatible with
 * strftime(3) that reflects the user's locale date preference.
 * The constants below are used as arguments to select the
 * date format style.
 * ================================================================ */

#define LIB$K_DATE_FORMAT_SHORT     1   /* Short date format (MM/DD/YY) */
#define LIB$K_DATE_FORMAT_MEDIUM    2   /* Medium format (DD-MMM-YYYY) */
#define LIB$K_DATE_FORMAT_LONG      3   /* Long format (Day, Month DD, YYYY) */
#define LIB$K_DATE_FORMAT_ISO       4   /* ISO 8601 format (YYYY-MM-DD) */

#ifdef __cplusplus
}
#endif

#endif /* __LIBDTDEF_H */
