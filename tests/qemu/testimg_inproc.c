/*
 * testimg_inproc.c - a minimal in-process-eligible OVMX image (vms-68f iv).
 *
 * NOT a test suite (its name does not match the test_*.c globs, so neither the
 * Dockerfile build recipes nor init.sh treat it as one). It is the SUBJECT
 * tests/qemu/test_syssvc_imgact_inproc.c activates in-process via
 * imgact_activate() (src/libvms/syssvc/sys_imgact.c) to prove RUN can run an
 * image in DCL's own process -- same PID, no fork.
 *
 * It is a freestanding, position-independent ET_DYN with NO PT_INTERP and no
 * symbolic relocations, entered through the OVMX in-process function-call ABI
 * `long img_entry(long mode, long addr)` (imgact_activate.h), and it carries
 * the OVMX in-process marker note that gates that path. It uses only stack
 * locals and inline syscalls, so it links with zero dynamic relocations.
 *
 *   mode == 0 : write a known banner to fd 1 and return 0 (the benign image).
 *   mode == 1 : additionally store to *addr -- used to prove that a critical-P1
 *               page mprotect'd read-only while the image runs faults instead
 *               of being corrupted (design §A.2.3(b)); the store never returns
 *               when the protection holds.
 */
#include <stdint.h>

#include "imgact_activate.h"   /* IMGACT_NOTE_OWNER / IMGACT_NOTE_TYPE */

#if defined(__x86_64__)
static long img_syscall3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}
#define IMG_SYS_write 1
#elif defined(__aarch64__)
static long img_syscall3(long n, long a, long b, long c)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
#define IMG_SYS_write 64
#else
static long img_syscall3(long n, long a, long b, long c)
{ (void)n; (void)a; (void)b; (void)c; return -1; }
#define IMG_SYS_write 0
#endif

/*
 * The OVMX in-process marker note (a PT_NOTE segment). Its presence is the
 * positive gate imgact_activate() checks; no real/foreign image carries it.
 * Layout: n_namesz, n_descsz, n_type, name[padded], desc.
 */
__attribute__((used, aligned(4), section(".note.ovmx")))
static const struct {
    uint32_t n_namesz;
    uint32_t n_descsz;
    uint32_t n_type;
    char     name[8];   /* "OVMX\0" padded to 4-byte multiple */
    uint32_t desc;
} img_ovmx_note = {
    5, 4, IMGACT_NOTE_TYPE, { 'O', 'V', 'M', 'X', 0, 0, 0, 0 }, 1u
};

long img_entry(long mode, long addr)
{
    static const char banner[] = "OVMX-INPROC-IMAGE-RAN\n";
    img_syscall3(IMG_SYS_write, 1, (long)banner, (long)(sizeof(banner) - 1));
    if (mode == 1)
        *(volatile long *)addr = 0xdead;   /* faults if addr is protected RO */
    return 0;
}
