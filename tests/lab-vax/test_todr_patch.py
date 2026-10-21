#!/usr/bin/env python3
"""test_todr_patch.py - host test for tools/cross-vax/netbsd-ovmx-todr.patch
(rd vms-b06).

THE DEFECT. The demo's Node B (OVMX/VAX under pcjs's KA655) booted at
1-JAN-2010 12:08 while the real OpenVMS VAX in the same page booted at today's
date; every SIMH acceptance boot did the same. A VAX TODR counts hundredths of
a second since 1 January and holds no year. NetBSD/vax's generic_gettime()
takes the year from the root filesystem's last write. That FFS was last written
by a build session whose own clock was NetBSD's built-in default
(kern_todr.c: 1-JAN-2010 12:00), so the TOY-derived time was 21-OCT-2010;
kern_todr.c rejects anything before 2021 as "preposterous" and falls back to
the filesystem time, 1-JAN-2010 12:08, with only a message-buffer warning. The
browser's TODR held the right day and time all along.

WHAT THIS PINS, by compiling the REAL NetBSD 10.1 generic_gettime(),
yeartonum(), numtoyear() and todr_set_systime() -- unpatched (fixtures/) and
patched -- on the host with stub kernel services, then booting them against a
simulated TODR:
  1. TEETH: unpatched, a 2010 filesystem year plus a valid 21-OCT TODR gives
     1-JAN-2010 12:08, and the kernel exposes no verdict.
  2. Patched, the same boot still has no year to trust, and says so:
     kern.ovmx_toy_valid = 0 (PID 1 then asks for the date, as VMS does).
  3. With a filesystem written in the current year (the build now sets the
     staging clock), the TODR gives today, verdict 1, patched and unpatched.
  4. Patched, a TODR earlier in the year than the filesystem's write is the
     next year (SIMH/pcjs load the TODR with the host's time of year);
     unpatched, NetBSD calls it "clock lost" and silently uses the old date.
  5. A stopped TODR, and a missing filesystem time, give verdict 0.
  6. The kernel build applies the patch idempotently, and the substrate cache
     keys and run-boot.sh's kernel stamp cover it (a stale kernel cannot be
     reused).

Run: pytest tests/lab-vax/test_todr_patch.py -v
"""

import calendar
import os
import re
import shutil
import subprocess
import tempfile

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PATCH = os.path.join(REPO, "tools/cross-vax/netbsd-ovmx-todr.patch")
CLOCK_UP = os.path.join(HERE, "fixtures/clock.c.netbsd-10")
TODR_UP = os.path.join(HERE, "fixtures/kern_todr.c.netbsd-10")
BUILD = os.path.join(REPO, "tools/cross-vax/build-vax-modular-kernel.sh")
RUNBOOT = os.path.join(HERE, "run-boot.sh")
WORKFLOWS = [os.path.join(REPO, ".github/workflows", w)
             for w in ("ci-vax.yml", "cluster-demo-nodeB.yml")]
CLOCK_REL = "sys/arch/vax/vax/clock.c"
TODR_REL = "sys/kern/kern_todr.c"

TODRBASE = 1 << 28

STUBS = r'''
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#define SECS_PER_DAY            86400
#define DAYS_PER_COMMON_YEAR    365
#define SECS_PER_COMMON_YEAR    (SECS_PER_DAY * DAYS_PER_COMMON_YEAR)
#define POSIX_BASE_YEAR         1970
#define days_per_year(y) ((((y) % 4 == 0 && (y) % 100 != 0) || (y) % 400 == 0) ? 366 : 365)
#define TODRBASE                (1 << 28)
#define PR_TODR                 27
#ifndef EINVAL
#define EINVAL                  22
#endif
#define KASSERT(x)              ((void)0)
#define rnd_add_data(a, b, c, d) ((void)0)
#define aprint_verbose          printf
#define SYSCTL_SETUP(name, desc) static void name(void *clog)
#define sysctl_createv(...)     ((void)0)
#define SYSCTL_DESCR(s)         s

static unsigned fake_todr;
#define mfpr(r)                 (fake_todr)

struct clock_ymdhms { int dt_year, dt_mon, dt_day, dt_wday, dt_hour, dt_min, dt_sec; };
static time_t clock_ymdhms_to_secs(struct clock_ymdhms *c)
{
    struct tm t; memset(&t, 0, sizeof t);
    t.tm_year = c->dt_year - 1900; t.tm_mon = c->dt_mon - 1; t.tm_mday = c->dt_day;
    t.tm_hour = c->dt_hour; t.tm_min = c->dt_min; t.tm_sec = c->dt_sec;
    return timegm(&t);
}

struct todr_chip_handle { time_t base_time; };
typedef struct todr_chip_handle *todr_chip_handle_t;
static struct todr_chip_handle the_chip;
static todr_chip_handle_t todr_handle = &the_chip;
static int generic_gettime(struct timeval *tvp);
/* vax_gettime() (clock.c): the base goes in, generic_gettime() fills in the rest */
static int todr_gettime(todr_chip_handle_t h, struct timeval *tvp)
{ tvp->tv_sec = h->base_time; return generic_gettime(tvp); }
static bool todr_lock_owned(void) { return true; }

static struct timespec set_clock;
static void tc_setclock(const struct timespec *ts) { set_clock = *ts; }
'''

MAIN = r'''
int main(int argc, char **argv)
{
    time_t base = (time_t)strtoll(argv[1], NULL, 10);
    fake_todr = (unsigned)strtoul(argv[2], NULL, 10);
    todr_set_systime(base);
    printf("\nRESULT %lld %d\n", (long long)set_clock.tv_sec, VALID);
    return 0;
}
'''


def func(src, rettype, name):
    m = re.search(r"\n%s\n%s\(.*?\n\}\n" % (re.escape(rettype), re.escape(name)), src, re.S)
    assert m, "%s() not found" % name
    return m.group(0)


def tree(patched):
    d = tempfile.mkdtemp(prefix="ovmx-todr-")
    for up, rel in ((CLOCK_UP, CLOCK_REL), (TODR_UP, TODR_REL)):
        dst = os.path.join(d, rel)
        os.makedirs(os.path.dirname(dst))
        shutil.copy(up, dst)
    if patched:
        subprocess.run(["patch", "-d", d, "-p1", "-s", "-i", PATCH], check=True)
    return d


def build(patched):
    d = tree(patched)
    clock = open(os.path.join(d, CLOCK_REL)).read()
    todr = open(os.path.join(d, TODR_REL)).read()
    prep = re.search(r"#define\tPREPOSTEROUS_YEARS.*\n", todr).group(0)
    parts = [STUBS, prep, "static bool timeset = false;\n"]
    if patched:
        m = re.search(r"static int todr_ovmx_toy_valid;\n", todr)
        assert m, "patched kern_todr.c lacks todr_ovmx_toy_valid"
        parts.append(m.group(0))
        parts.append("#define VALID todr_ovmx_toy_valid\n")
    else:
        assert "ovmx_toy_valid" not in todr
        parts.append("#define VALID -1\n")
    for name in ("yeartonum", "numtoyear", "generic_gettime"):
        parts.append("static " + func(clock, "int", name).lstrip("\n"))
    parts.append("static " + func(todr, "void", "todr_set_systime").lstrip("\n"))
    parts.append(MAIN)
    c = os.path.join(d, "harness.c")
    open(c, "w").write("".join(parts))
    exe = os.path.join(d, "harness")
    subprocess.run(["cc", "-std=gnu11", "-w", "-o", exe, c], check=True)
    return d, exe


@pytest.fixture(scope="module")
def unpatched():
    d, exe = build(False)
    yield exe
    shutil.rmtree(d, ignore_errors=True)


@pytest.fixture(scope="module")
def patched():
    d, exe = build(True)
    yield exe
    shutil.rmtree(d, ignore_errors=True)


def utc(*a):
    return calendar.timegm(tuple(a) + (0,) * (6 - len(a)))


def todr_for(*a):
    """The TODR SIMH/pcjs load at power on: hundredths since 1 January."""
    t = utc(*a)
    return TODRBASE + (t - utc(a[0], 1, 1)) * 100


def boot(exe, base, todr):
    out = subprocess.run([exe, str(base), str(todr)], check=True,
                         capture_output=True, text=True).stdout
    m = re.search(r"RESULT (-?\d+) (-?\d+)", out)
    return int(m.group(1)), int(m.group(2)), out


FS_2010 = utc(2010, 1, 1, 12, 8)        # the Node B root FFS's last write
TODAY = (2026, 10, 21, 15, 6, 27)       # Node C's "startup begun" in the same page


def test_teeth_unpatched_boots_2010_with_no_verdict(unpatched):
    t, valid, out = boot(unpatched, FS_2010, todr_for(*TODAY))
    assert t == FS_2010, "the defect: 1-JAN-2010 12:08 despite a 21-OCT TODR"
    assert "preposterous TOD clock time" in out
    assert valid == -1


def test_patched_no_year_says_invalid(patched):
    t, valid, out = boot(patched, FS_2010, todr_for(*TODAY))
    assert t == FS_2010
    assert valid == 0, "kern.ovmx_toy_valid must be 0 so PID 1 asks for the date"


def test_current_year_base_gives_today(patched, unpatched):
    build_time = utc(2026, 10, 19, 4, 30)
    for exe in (patched, unpatched):
        t, valid, _ = boot(exe, build_time, todr_for(*TODAY))
        assert t == utc(*TODAY)
    assert boot(patched, build_time, todr_for(*TODAY))[1] == 1


def test_same_day_earlier_hour_is_not_a_new_year(patched):
    # a browser west of UTC: its time of year is hours behind the UTC base
    base = utc(2026, 10, 21, 15, 0)
    t, valid, _ = boot(patched, base, todr_for(2026, 10, 21, 8, 0))
    assert (t, valid) == (utc(2026, 10, 21, 8, 0), 1)


def test_year_rollover(patched, unpatched):
    base = utc(2026, 10, 19, 4, 30)
    jan = (2027, 1, 5, 10, 0)
    t, valid, _ = boot(patched, base, todr_for(*jan))
    assert (t, valid) == (utc(*jan), 1)
    t, valid, out = boot(unpatched, base, todr_for(*jan))
    assert t == base and "clock lost" in out


def test_stopped_todr_invalid(patched):
    base = utc(2026, 10, 19, 4, 30)
    t, valid, _ = boot(patched, base, 0)
    assert valid == 0


def test_no_filesystem_time_invalid(patched):
    t, valid, _ = boot(patched, 0, todr_for(*TODAY))
    assert valid == 0 and t == utc(2010, 1, 1, 12, 0)


def test_build_applies_patch_idempotently():
    src = open(BUILD).read()
    assert "netbsd-ovmx-todr.patch" in src
    assert "grep -q 'ovmx_toy_valid' \"$SRC/sys/kern/kern_todr.c\"" in src


def test_stale_kernel_cannot_be_reused():
    rb = open(RUNBOOT).read()
    stamp = re.search(r'KERNEL_STAMP="\$\(cat (.*?)\| sha256sum', rb, re.S).group(1)
    assert "netbsd-ovmx-todr.patch" in stamp
    for w in WORKFLOWS:
        for key in re.findall(r"key: vax-substrate-.*", open(w).read()):
            assert "netbsd-ovmx-todr.patch" in key, w


def test_build_session_gives_the_disk_todays_year():
    """Every filesystem the shipped disk's build session writes takes its year
    from that session's clock; it must be set from the host (UTC), not left at
    NetBSD's 1-JAN-2010."""
    src = open(os.path.join(HERE, "drive_boot_vax.py")).read()
    m = re.search(r"\ndef start_single_user\(.*?\n(?=def )", src, re.S)
    assert m, "start_single_user() not found"
    body = m.group(0)
    assert re.search(r'"date -u %s" % time\.strftime\("%Y%m%d%H%M\.%S", time\.gmtime\(\)\)', body)
    # set after the root is writable, before the session hands back its shell
    assert body.index("mount -u -w /") < body.index('"date -u') < body.index("set_unique_prompt")
