/*
 * __tz.c - the C RTL's time zone is the VMS system's (vms-593).
 *
 * musl 1.2.5's src/time/__tz.c with ONE change: the zone description comes from
 * the VMS time-zone logical names, never from the TZ environment variable or a
 * zoneinfo file. The DEC C RTL on OpenVMS takes the local time zone from the
 * system's SYS$TIMEZONE_DIFFERENTIAL / SYS$TIMEZONE_NAME / SYS$TIMEZONE_RULE
 * logicals; src/vmsrms/crtl_time.c translates them (through $TRNLNM) into the
 * POSIX zone string the rule code below parses, through __ovmx_tz_hook. The
 * logicals are re-read on every conversion, so a DEFINE of
 * SYS$TIMEZONE_DIFFERENTIAL moves the next localtime() of a running image.
 *
 * Without the hook (the bootstrap DECC$SHR, which links no system service)
 * there is no VMS zone to read, and the zone is UTC. No path here opens a file.
 * The same file is the x86_64 static rig's overlay (src/crtl-musl-x86_64).
 */
#include "../time_impl.h"   /* src/time/, from the per-arch directory */
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ctype.h>
#include "libc.h"
#include "lock.h"
#include "fork_impl.h"

#define malloc __libc_malloc
#define calloc undef
#define realloc undef
#define free undef

long  __timezone = 0;
int   __daylight = 0;
char *__tzname[2] = { 0, 0 };

weak_alias(__timezone, timezone);
weak_alias(__daylight, daylight);
weak_alias(__tzname, tzname);

static char std_name[TZNAME_MAX+1];
static char dst_name[TZNAME_MAX+1];
const char __utc[] = "UTC";

static int dst_off;
static int r0[5], r1[5];

static const unsigned char *zi, *trans, *index, *types, *abbrevs, *abbrevs_end;
static size_t map_size;

static char old_tz_buf[96];
static char *old_tz = old_tz_buf;
static size_t old_tz_size = sizeof old_tz_buf;

static volatile int lock[1];
volatile int *const __timezone_lockptr = lock;

static int getint(const char **p)
{
	unsigned x;
	for (x=0; **p-'0'<10U; (*p)++) x = **p-'0' + 10*x;
	return x;
}

static int getoff(const char **p)
{
	int neg = 0;
	if (**p == '-') {
		++*p;
		neg = 1;
	} else if (**p == '+') {
		++*p;
	}
	int off = 3600*getint(p);
	if (**p == ':') {
		++*p;
		off += 60*getint(p);
		if (**p == ':') {
			++*p;
			off += getint(p);
		}
	}
	return neg ? -off : off;
}

static void getrule(const char **p, int rule[5])
{
	int r = rule[0] = **p;

	if (r!='M') {
		if (r=='J') ++*p;
		else rule[0] = 0;
		rule[1] = getint(p);
	} else {
		++*p; rule[1] = getint(p);
		++*p; rule[2] = getint(p);
		++*p; rule[3] = getint(p);
	}

	if (**p=='/') {
		++*p;
		rule[4] = getoff(p);
	} else {
		rule[4] = 7200;
	}
}

static void getname(char *d, const char **p)
{
	int i;
	if (**p == '<') {
		++*p;
		for (i=0; (*p)[i] && (*p)[i]!='>'; i++)
			if (i<TZNAME_MAX) d[i] = (*p)[i];
		if ((*p)[i]) ++*p;
	} else {
		for (i=0; ((*p)[i]|32)-'a'<26U; i++)
			if (i<TZNAME_MAX) d[i] = (*p)[i];
	}
	*p += i;
	d[i<TZNAME_MAX?i:TZNAME_MAX] = 0;
}

#define VEC(...) ((const unsigned char[]){__VA_ARGS__})

static uint32_t zi_read32(const unsigned char *z)
{
	return (unsigned)z[0]<<24 | z[1]<<16 | z[2]<<8 | z[3];
}

static size_t zi_dotprod(const unsigned char *z, const unsigned char *v, size_t n)
{
	size_t y;
	uint32_t x;
	for (y=0; n; n--, z+=4, v++) {
		x = zi_read32(z);
		y += x * *v;
	}
	return y;
}

/* Fills buf with a POSIX zone string from the VMS time-zone logicals and
 * returns nonzero, or returns 0 when there is none (UTC). */
size_t (*__ovmx_tz_hook)(char *, size_t) = 0;
static char vms_tz_buf[96];

static void do_tzset()
{
	const char *s, *p;
	size_t i;

	s = __utc;
	if (__ovmx_tz_hook && __ovmx_tz_hook(vms_tz_buf, sizeof vms_tz_buf)) {
		vms_tz_buf[sizeof vms_tz_buf - 1] = 0;
		s = vms_tz_buf;
	}

	if (old_tz && !strcmp(s, old_tz)) return;

	for (i=0; i<5; i++) r0[i] = r1[i] = 0;

	i = strlen(s);
	if (i >= old_tz_size) s = __utc, i = 3;
	memcpy(old_tz, s, i+1);

	/* Only the POSIX form: a VMS zone is never a file name. */
	p = s;
	char dummy_name[TZNAME_MAX+1];
	getname(dummy_name, &p);
	if (!(p!=s && (*p == '+' || *p == '-' || isdigit(*p)
	               || !strcmp(dummy_name, "UTC")
	               || !strcmp(dummy_name, "GMT"))))
		s = __utc;

	zi = 0;

	if (!s) s = __utc;
	getname(std_name, &s);
	__tzname[0] = std_name;
	__timezone = getoff(&s);
	getname(dst_name, &s);
	__tzname[1] = dst_name;
	if (dst_name[0]) {
		__daylight = 1;
		if (*s == '+' || *s=='-' || *s-'0'<10U)
			dst_off = getoff(&s);
		else
			dst_off = __timezone - 3600;
	} else {
		__daylight = 0;
		dst_off = __timezone;
	}

	if (*s == ',') s++, getrule(&s, r0);
	if (*s == ',') s++, getrule(&s, r1);
}

/* Search zoneinfo rules to find the one that applies to the given time,
 * and determine alternate opposite-DST-status rule that may be needed. */

static size_t scan_trans(long long t, int local, size_t *alt)
{
	int scale = 3 - (trans == zi+44);
	uint64_t x;
	int off = 0;

	size_t a = 0, n = (index-trans)>>scale, m;

	if (!n) {
		if (alt) *alt = 0;
		return 0;
	}

	/* Binary search for 'most-recent rule before t'. */
	while (n > 1) {
		m = a + n/2;
		x = zi_read32(trans + (m<<scale));
		if (scale == 3) x = x<<32 | zi_read32(trans + (m<<scale) + 4);
		else x = (int32_t)x;
		if (local) off = (int32_t)zi_read32(types + 6 * index[m-1]);
		if (t - off < (int64_t)x) {
			n /= 2;
		} else {
			a = m;
			n -= n/2;
		}
	}

	/* First and last entry are special. First means to use lowest-index
	 * non-DST type. Last means to apply POSIX-style rule if available. */
	n = (index-trans)>>scale;
	if (a == n-1) return -1;
	if (a == 0) {
		x = zi_read32(trans);
		if (scale == 3) x = x<<32 | zi_read32(trans + 4);
		else x = (int32_t)x;
		/* Find the lowest non-DST type, or 0 if none. */
		size_t j = 0;
		for (size_t i=abbrevs-types; i; i-=6) {
			if (!types[i-6+4]) j = i-6;
		}
		if (local) off = (int32_t)zi_read32(types + j);
		/* If t is before first transition, use the above-found type
		 * and the index-zero (after transition) type as the alt. */
		if (t - off < (int64_t)x) {
			if (alt) *alt = index[0];
			return j/6;
		}
	}

	/* Try to find a neighboring opposite-DST-status rule. */
	if (alt) {
		if (a && types[6*index[a-1]+4] != types[6*index[a]+4])
			*alt = index[a-1];
		else if (a+1<n && types[6*index[a+1]+4] != types[6*index[a]+4])
			*alt = index[a+1];
		else
			*alt = index[a];
	}

	return index[a];
}

static int days_in_month(int m, int is_leap)
{
	if (m==2) return 28+is_leap;
	else return 30+((0xad5>>(m-1))&1);
}

/* Convert a POSIX DST rule plus year to seconds since epoch. */

static long long rule_to_secs(const int *rule, int year)
{
	int is_leap;
	long long t = __year_to_secs(year, &is_leap);
	int x, m, n, d;
	if (rule[0]!='M') {
		x = rule[1];
		if (rule[0]=='J' && (x < 60 || !is_leap)) x--;
		t += 86400 * x;
	} else {
		m = rule[1];
		n = rule[2];
		d = rule[3];
		t += __month_to_secs(m-1, is_leap);
		int wday = (int)((t + 4*86400) % (7*86400)) / 86400;
		int days = d - wday;
		if (days < 0) days += 7;
		if (n == 5 && days+28 >= days_in_month(m, is_leap)) n = 4;
		t += 86400 * (days + 7*(n-1));
	}
	t += rule[4];
	return t;
}

/* Determine the time zone in effect for a given time in seconds since the
 * epoch. It can be given in local or universal time. The results will
 * indicate whether DST is in effect at the queried time, and will give both
 * the GMT offset for the active zone/DST rule and the opposite DST. This
 * enables a caller to efficiently adjust for the case where an explicit
 * DST specification mismatches what would be in effect at the time. */

void __secs_to_zone(long long t, int local, int *isdst, long *offset, long *oppoff, const char **zonename)
{
	LOCK(lock);

	do_tzset();

	if (zi) {
		size_t alt, i = scan_trans(t, local, &alt);
		if (i != -1) {
			*isdst = types[6*i+4];
			*offset = (int32_t)zi_read32(types+6*i);
			*zonename = (const char *)abbrevs + types[6*i+5];
			if (oppoff) *oppoff = (int32_t)zi_read32(types+6*alt);
			UNLOCK(lock);
			return;
		}
	}

	if (!__daylight) goto std;

	/* FIXME: may be broken if DST changes right at year boundary?
	 * Also, this could be more efficient.*/
	long long y = t / 31556952 + 70;
	while (__year_to_secs(y, 0) > t) y--;
	while (__year_to_secs(y+1, 0) < t) y++;

	long long t0 = rule_to_secs(r0, y);
	long long t1 = rule_to_secs(r1, y);

	if (!local) {
		t0 += __timezone;
		t1 += dst_off;
	}
	if (t0 < t1) {
		if (t >= t0 && t < t1) goto dst;
		goto std;
	} else {
		if (t >= t1 && t < t0) goto std;
		goto dst;
	}
std:
	*isdst = 0;
	*offset = -__timezone;
	if (oppoff) *oppoff = -dst_off;
	*zonename = __tzname[0];
	UNLOCK(lock);
	return;
dst:
	*isdst = 1;
	*offset = -dst_off;
	if (oppoff) *oppoff = -__timezone;
	*zonename = __tzname[1];
	UNLOCK(lock);
}

static void __tzset()
{
	LOCK(lock);
	do_tzset();
	UNLOCK(lock);
}

weak_alias(__tzset, tzset);

const char *__tm_to_tzname(const struct tm *tm)
{
	const void *p = tm->__tm_zone;
	LOCK(lock);
	do_tzset();
	if (p != __utc && p != __tzname[0] && p != __tzname[1] &&
	    (!zi || (uintptr_t)p-(uintptr_t)abbrevs >= abbrevs_end - abbrevs))
		p = "";
	UNLOCK(lock);
	return p;
}
