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
 * imgact_activate() takes the in-process path ONLY for an image that carries
 * the OVMX in-process marker note (IMGACT_NOTE_*), has no PT_INTERP, no PT_TLS,
 * and only R_*_RELATIVE dynamic relocations -- i.e. a position-independent OVMX
 * image entered through the (a0,a1) function-call ABI below. Such an image MAY
 * now import universals from an already-resident shareable through a .vms$imp
 * table: those imports are bound to the RESIDENT producer via the registry in
 * imgact_prodreg.h (vms-db2), never a private copy. An import naming a producer
 * that is NOT resident, or any image with a PT_INTERP / PT_TLS / the SysV auxv
 * entry ABI (a real DCL utility, a LINK.EXE _start image), returns
 * SS$_UNSUPPORTED so the caller keeps the fork model (design §A.6.6 / §A.8
 * remainder: B stays the RUN fallback until IMGACT publishes the resident
 * registry at DCL startup and the auxv-entry/exit + shared-TLS pieces land, at
 * which point real images flip in-process). With no /dev/vms it returns
 * SS$_NOSUCHDEV and refuses to run the image at all (INV-6: no per-process fake
 * of an executive facility).
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

#endif /* _IMGACT_ACTIVATE_H */
