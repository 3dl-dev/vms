/*
 * p4boot_rt.c -- vms-d4a (vms-404 P4) minimal NetBSD-libc-backed runtime shim,
 * built as its own tiny elf32-vax `.vms$sv` OVMX shareable (P4BOOT$SHR.EXE).
 *
 * WHY THIS EXISTS. A LINKVAX.EXE `--executable` consumer's synthesized crt0
 * ALWAYS tail-calls an imported `exit` universal (src/vmslink/link.c: "An
 * executable's synthesized crt0 ... tail-calls exit() to flush stdio and
 * terminate"; LINKVAX.EXE dies at link time -- "internal: exit import missing
 * for crt0" -- if no `--use` producer exports it). The SHIPPED LIBVMS$SHR.EXE
 * (tools/cross-vax/build-vax-shareable-graph.sh, rd vms-c7f7) is the REAL
 * src/libvms RTL: it does not export a raw process-termination primitive, and
 * it has no console-write primitive either (those are not RTL surface). This
 * tiny SEPARATE producer supplies both, purpose-built for the P4 runtime-
 * activation gate (rd vms-d4a); it is staged ALONGSIDE the shipped LIBVMS$SHR
 * on SYS$SHARE and never modifies that shipped packaging.
 *
 * Ordinary NetBSD-libc-backed C (NOT freestanding, unlike the consumer's
 * vaxcons_main.c): it compiles normally against the vax--netbsdelf sysroot,
 * so `write`/`_exit` resolve as ordinary calls that the selective NetBSD
 * libc.OLB pull (build-shr-activation-vax.sh) defines in the final
 * shareable -- exactly the mechanism build-vax-shareable-graph.sh already
 * uses for LIBVMS$SHR.EXE's own libc dependencies.
 */
#include <unistd.h>

/* `exit` -- the crt0's REQUIRED tail-call target (src/vmslink/link.c). Real
 * process termination (not a build-only no-op stub, unlike the P3a/vms-099
 * readelf-shape test's throwaway producer): NetBSD/vax's raw _exit(2)
 * syscall, so a genuine SIMH-booted consumer actually terminates. */
void exit(int code)
{
	_exit(code);
}

/*
 * p4boot_puts -- write a NUL-terminated ASCII line to the console (fd 1).
 *
 * This is the value-sensitive channel the P4 gate's assertion reads from the
 * SIMH console transcript: VAX consumer images are activated SysV-flavor
 * (docs comment, src/imgact/arch/vax/start.S: "mirror x86_64/aarch64's SysV
 * tail-jump, NOT Alpha's PDSC trampoline"), and IMGACT's VMS-standard
 * $STATUS/OVMX-SEAM instrumentation (imgact_vms_standard_activate,
 * src/imgact/imgact.c) is explicitly Alpha-only ("VMS-standard image
 * activation is Alpha-only" -- fails honest on every other arch). A SysV
 * image's $STATUS only ever collapses to SS$_NORMAL / SS$_ABORT / SS$_ACCVIO
 * (src/libvms/syssvc/sys_imgact.c, imgact_activate()'s rundown comment) --
 * never a value-sensitive encoding. So the value-sensitive proof that the
 * cross-shareable call into LIBVMS$SHR.EXE really ran is the CONSUMER
 * printing its own result, over a REAL write(2) syscall, captured on the
 * SIMH console exactly like the boot's other milestone lines.
 */
long p4boot_puts(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return (long)write(1, s, n);
}
