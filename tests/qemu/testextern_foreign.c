/*
 * testextern_foreign.c - the FOREIGN-interpreter negative control for the
 * external-image flip (vms-db2, §A.8 remainder item 2). NOT a test suite (no
 * test_*.c glob match); a SUBJECT test_syssvc_imgact_extern.c activates.
 *
 * This image is in-process-eligible in EVERY respect the flip cares about -- it
 * carries the OVMX in-process marker note (AUXV ABI), has no own PT_TLS, no
 * symbolic reloc, and imports nothing -- EXCEPT that its PT_INTERP names a
 * FOREIGN loader ("/lib/ld-musl-x86_64.so.1"), not the OVMX loader. The ONLY
 * reason imgact_activate must refuse it (SS$_UNSUPPORTED -> the caller forks) is
 * that foreign interp. It is the anti-over-acceptance guard: the external flip
 * accepts a PT_INTERP that names IMGACT.EXE precisely BECAUSE in-process
 * activation is that loader; it must NOT run a foreign-interpreted image
 * in-process (that would be a LARP -- an image whose real loader we skipped).
 *
 * If imgact ever accepted this image in-process it would enter extern_start and
 * print OVMX-FOREIGN-RAN; the suite asserts that banner is ABSENT and the status
 * is SS$_UNSUPPORTED. Rule 8: ELF/PT_INTERP mechanics are the public System V ABI.
 */
#include <stdint.h>

#include "imgact_activate.h"   /* IMGACT_NOTE_* / IMGACT_ABI_* */

/* A FOREIGN interpreter -- deliberately NOT the OVMX loader (IMGACT.EXE). */
__attribute__((used, section(".interp")))
static const char foreign_interp[] = "/lib/ld-musl-x86_64.so.1";

/* OVMX in-process marker (AUXV ABI), so the ONLY disqualifier is the interp. */
__attribute__((used, aligned(4), section(".note.ovmx")))
static const struct {
    uint32_t n_namesz;
    uint32_t n_descsz;
    uint32_t n_type;
    char     name[8];
    uint32_t desc;
} foreign_ovmx_note = {
    5, 4, IMGACT_NOTE_TYPE, { 'O', 'V', 'M', 'X', 0, 0, 0, 0 },
    IMGACT_ABI_MARKER | IMGACT_ABI_AUXV
};

#if defined(__x86_64__)
static long img_write(long fd, const void *buf, long n)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(1L), "D"(fd), "S"(buf), "d"(n)
                     : "rcx", "r11", "memory");
    return r;
}
static void img_exit_group(long code)
{
    __asm__ volatile("syscall" : : "a"(231L), "D"(code) : "memory");
    __builtin_unreachable();
}
#elif defined(__aarch64__)
static long img_write(long fd, const void *buf, long n)
{
    register long x8 __asm__("x8") = 64;
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = n;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
static void img_exit_group(long code)
{
    register long x8 __asm__("x8") = 94;
    register long x0 __asm__("x0") = code;
    __asm__ volatile("svc #0" : : "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}
#else
static long img_write(long fd, const void *buf, long n){(void)fd;(void)buf;(void)n;return -1;}
static void img_exit_group(long code){(void)code; for(;;){}}
#endif

/* This body must NEVER run (imgact refuses the image at the foreign interp,
 * before entering it). If it does, the banner makes the LARP visible. */
__attribute__((visibility("hidden"))) long foreign_main(long argc);
__attribute__((visibility("hidden"))) long foreign_main(long argc)
{
    static const char banner[] = "OVMX-FOREIGN-RAN\n";
    img_write(1, banner, (long)(sizeof(banner) - 1));
    img_exit_group(argc);
    return 0;
}

__asm__(
#if defined(__x86_64__)
    ".text\n.globl extern_start\n.type extern_start,@function\n"
    "extern_start:\n"
    "  mov (%rsp), %rdi\n"
    "  call foreign_main\n"
    "  ud2\n"
    ".size extern_start,.-extern_start\n"
#elif defined(__aarch64__)
    ".text\n.globl extern_start\n.type extern_start,@function\n"
    "extern_start:\n"
    "  ldr x0, [sp]\n"
    "  bl foreign_main\n"
    "  brk #0\n"
    ".size extern_start,.-extern_start\n"
#endif
);
