/*
 * hello_vms_derisk.c — vms-530 day-1 GO/NO-GO de-risk scratch program.
 *
 * NOT a shipped OVMX image and NOT part of any CMake/link_native_graph
 * target. This is the trivial "hello, VMS" main()-based C program vms-530
 * asks for: compiled with the same musl/freestanding flags every mk_*_shr.sh
 * producer/consumer in the b6a/b65 lib-migration chain uses, then linked
 * by the REAL LINK.EXE (src/vmslink/link.c) as a VMS-native --executable
 * (ET_DYN, PT_INTERP=IMGACT.EXE, crt0 synthesized by emit_executable) that
 * imports printf/getuid/geteuid from the REAL DECC$SHR.EXE producer (the
 * same shareable DCL.EXE and every other OVMX-native image binds against —
 * see mk_decc_shr.sh) via a genuine cross-image CALL, not a bespoke test
 * shareable.
 *
 * Purpose: prove, by ground-source QEMU execution, whether a trivial
 * LINK.EXE-built .EXE activates through IMGACT.EXE when the calling process
 * is NOT root. printf's banner is the activation proof; the getuid/geteuid
 * line is the non-root proof — root ownership of /vms (mastered at image
 * build time, same as /usr on any Linux distro) is orthogonal to whether
 * the ACTIVATING PROCESS needs to be root, which is the actual vms-0b8
 * question. See docs/derisk-vms-530-imgact-qemu.md for the verdict.
 *
 * Hand-declared prototypes (no <stdio.h>/<unistd.h>) to match this
 * translation unit's -ffreestanding build (mk_dcl.sh's DCL.EXE flags):
 * freestanding mode does not guarantee libc headers, only compiler builtins,
 * so the externs are spelled out exactly as DECC$SHR's producer vector
 * defines them (mk_decc_shr.sh: printf/getuid/geteuid all PROCEDURE
 * universals).
 */

extern int printf(const char *fmt, ...);
extern unsigned int getuid(void);
extern unsigned int geteuid(void);

int main(void)
{
    printf("%%HELLOVMS-I-ACTIVATED, hello, VMS! (native LINK.EXE + IMGACT.EXE)\n");
    printf("%%HELLOVMS-I-IDENT, uid=%u euid=%u\n", getuid(), geteuid());
    return 0;
}
