/*
 * imgact_activate.h - in-process image activation (SYS$IMGACT as a library
 * inside DCL), vms-68f increment (iv).
 *
 * docs/design-in-process-activation.md Part II §A.2.1, §A.2.4: on OpenVMS,
 * RUN / a foreign command / a DCL utility does NOT create a process -- the
 * image is mapped into the CURRENT process's P0 region and runs there with
 * the process's PID, then image rundown returns control to DCL in the SAME
 * process. OVMX has always fork()+execve()'d a fresh Linux process per image
 * (src/vmsdcl/dcl_cmd_process.c); this is the library that runs an image
 * IN-PROCESS instead -- no fork, same VMS PID, entered in User mode across a
 * vms.ko-mediated Supervisor->User transition. (Increment iv enters the image
 * with a direct call and recovers a fault with setjmp/longjmp; the separate P0
 * User stack + swapcontext return path the design prefers waits for increment
 * v -- the ucontext family is not a DECC$SHR universal, so the fork
 * replacement must avoid it to link into the VMS-native DCL.EXE. See
 * src/libvms/syssvc/sys_imgact.c's header.)
 *
 * SCOPE -- a PROVEN PARTIAL, honest about its ceiling.
 * imgact_activate() takes the in-process path for an image that carries the
 * OVMX in-process marker note (IMGACT_NOTE_*), has no own PT_TLS, and only
 * R_*_RELATIVE dynamic relocations (no symbolic PLT). It is entered either
 * through the increment-iv (a0,a1) function-call ABI or, for a REAL image, the
 * SysV auxv `_start` ABI (IMGACT_ABI_AUXV). Such an image MAY import universals
 * from an already-resident shareable through a .vms$imp table: those imports are
 * bound to the RESIDENT producer via the registry in imgact_prodreg.h (vms-db2),
 * never a private copy.
 *
 * PT_INTERP (vms-db2, the EXTERNAL-image flip, §A.8 remainder item 2). A
 * genuinely external LINK.EXE image names its loader in PT_INTERP
 * (src/vmslink/link.c emits "/vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE"). In-process
 * activation IS that loader, so an interp whose basename is the OVMX loader
 * (IMGACT.EXE) does NOT disqualify the image -- imgact_activate does the
 * interpreter's job in DCL's own process. An interp naming a FOREIGN loader (a
 * real ld.so, a #! shell) is refused SS$_UNSUPPORTED so the caller forks.
 *
 * Still refused (SS$_UNSUPPORTED -> caller forks): an image with its OWN PT_TLS,
 * a symbolic (PLT) reloc, or a .vms$imp import naming a NON-resident producer --
 * the full 55 KB loader re-homing (PT_TLS/DTV append, in-process mapping of a
 * non-resident producer graph) is the deferred remainder (design §A.8 item 1/4).
 * With no /dev/vms it returns SS$_NOSUCHDEV and refuses to run the image at all
 * (INV-6: no per-process fake of an executive facility).
 *
 * THE ENTRY ABI (OVMX design choice, Rule 8 -- no VMS byte format claimed):
 * an eligible image is entered as `long entry(long a0, long a1)`; a0/a1 are
 * passed straight through from this call. It returns to DCL by RETURNING
 * (rundown then the call returns), never by exiting the process.
 */
#ifndef _IMGACT_ACTIVATE_H
#define _IMGACT_ACTIVATE_H

#include <stdint.h>

/* OVMX in-process marker note (a PT_NOTE segment): owner "OVMX", type below.
 * Its presence is the positive gate that an image uses the in-process
 * function-call entry ABI -- no real/foreign image carries it, so wiring
 * imgact_activate() into RUN cannot change how any existing image activates. */
#define IMGACT_NOTE_OWNER "OVMX"
#define IMGACT_NOTE_TYPE  0x4f564d58u   /* 'O','V','M','X' */

/*
 * The note's 4-byte descriptor is a flag word declaring the image's entry ABI
 * (vms-db2, §A.8 remainder gap 2a). Bit 0 is the in-process marker itself (set
 * on every in-process-eligible image); bit 1 (IMGACT_ABI_AUXV) declares the
 * image is entered through the SysV auxv `_start` ABI -- a REAL image's entry:
 * the activator constructs the initial process stack (argc/argv/envp/auxv) and
 * jumps to `_start`, which reads it exactly as the kernel-launched case. An
 * image WITHOUT bit 1 uses increment iv's (a0,a1) function-call ABI (the marker
 * TESTIMG.EXE). imgact_activate() reads this to pick the entry path, so wiring
 * the auxv path in cannot change how an existing (a0,a1) marker image activates.
 */
#define IMGACT_ABI_MARKER 0x1u   /* desc bit 0: in-process-eligible (always set) */
#define IMGACT_ABI_AUXV   0x2u   /* desc bit 1: entered via SysV auxv _start     */

/*
 * A page-aligned range in the CALLER's own address space (DCL's crown-jewel
 * P1 structures, design §A.2.3(b)) to mprotect() read-only while the image
 * runs in User mode, restored to read/write at rundown. Optional: pass NULL
 * to protect nothing.
 */
struct imgact_critp1 {
    uint64_t base;
    uint64_t limit;
};

/*
 * Activate `path` IN THE CURRENT PROCESS. Returns a VMS status:
 *   SS$_NORMAL       image ran in-process and returned to DCL (same PID)
 *   SS$_ACCVIO       image took an access violation (e.g. it wrote a
 *                    protected critical-P1 page); it was run down and DCL
 *                    survived -- exactly what VMS does with an ACCVIO in an
 *                    image (design §A.6.3, pragmatic SIGSEGV->rundown)
 *   SS$_UNSUPPORTED  not an in-process-eligible image -- caller uses fork
 *   SS$_NOSUCHDEV    no /dev/vms -- executive absent, image NOT run (INV-6)
 *   SS$_IVIMGFMT/... a malformed image
 * *image_rc (optional) receives the image entry's return value on SS$_NORMAL.
 */
uint32_t imgact_activate(const char *path, long a0, long a1,
                         const struct imgact_critp1 *critp1,
                         int *image_rc);

/*
 * SYS$EXIT for an IN-PROCESS image (vms-db2, §A.8 remainder gap 2b -- "intercept
 * SYS$EXIT to return to DCL instead of terminating the process").
 *
 * On OpenVMS a real image ends by calling SYS$EXIT, which runs image rundown and
 * returns control to the CLI in the SAME process. OVMX's native crt0 ends an
 * image with a raw `exit_group` syscall (src/libvmssys/arch/<arch>/crt0.S) --
 * fatal
 * for an in-process image, because it would terminate DCL too. A real image
 * activated in-process therefore calls THIS routine (a LIBVMS$SHR universal it
 * imports from the resident shareable, exactly as it imports every other
 * service) instead: when an in-process activation is open, it does NOT exit the
 * process -- it unwinds back to imgact_activate() (via the setjmp/longjmp path,
 * design §A.6.4 option (b)), which runs the image down and returns to DCL. When
 * no in-process activation is open (a forked/normal image called it), it exits
 * the process with `code`, so the routine is safe in both worlds.
 *
 * This is the faithful SYS$EXIT->return-to-DCL crux of the real-image flip: the
 * image reaches it through resident-producer import binding (imgact_prodreg.h),
 * so the SAME resident routine DCL's process holds is what unwinds the image.
 */
void imgact_image_exit(int code);

#endif /* _IMGACT_ACTIVATE_H */
