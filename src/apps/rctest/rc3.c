/*
 * rc3.c - a minimal RUN target for the DCL RUN-path acceptance gate (vms-707).
 *
 * The image prints one line to SYS$OUTPUT and exits successfully. RUN's fork
 * path was reworked (waitid(WNOWAIT) peek -> executive $STATUS readback by Linux
 * pid -> reap); this image is the end-to-end smoke test on the real runtime that
 * the reworked path still (a) activates the image, (b) routes its stdout to the
 * console, and (c) reports the image's completion status -- SS$_NORMAL for a
 * clean exit, read back through the new path, never a hang or a wrong status.
 *
 * The FAITHFUL-ENCODING half of vms-707 -- a bit<0>-set completion condition
 * (C$_EXIT1 + (N-1)*8) surviving to DCL's $STATUS instead of collapsing to
 * %X00000001 -- is exercised by the Alpha GCC-port images (crtl_rms), whose
 * IMGACT VMS-standard activation records the condition and releases the channel
 * before exit; x86_64 has no such activation path (imgact.c is a stub there), so
 * that half is proven at the executive level by tests/qemu/test_kmod_exit.c
 * (Part 5, real /dev/vms) and end-to-end by the Alpha crtl_rms re-run.
 *
 * Built with the same cc -> LINK.EXE toolchain as PARTS.EXE (VMS-native ET_DYN,
 * PT_INTERP=IMGACT.EXE), so it activates through the real image activator over
 * the ACP exactly as any RUN target does.
 */
#include <stdio.h>

int main(void)
{
    printf("RC3: image output reached SYS$OUTPUT (RUN routed the child stdout)\n");
    fflush(stdout);
    return 0;
}
