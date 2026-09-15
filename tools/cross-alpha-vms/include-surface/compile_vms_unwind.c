/* compile_vms_unwind.c - include-surface proof wrapper for vms-unwind.h
 * (vms-8e8c, CHF rung-5).
 *
 * This wrapper supplies the libgcc unwinder ABI (the scaffolding
 * libgcc/unwind-dw2.c would have supplied - see libgcc_unwind_harness.h) and
 * then #includes the GENUINE, UNPATCHED upstream
 * libgcc/config/alpha/vms-unwind.h (GCC 14.2.0, SHA256-pinned, extracted at
 * proof time from the vendored gcc-14.2.0.tar.xz). The port source is included
 * VERBATIM: this file adds nothing to it and patches nothing in it. Its own
 * four #includes - <vms/pdscdef.h>, <vms/libicb.h>, <vms/chfctxdef.h>,
 * <vms/chfdef.h> - must resolve against src/libvms/include's vms/ shim, and the
 * fields it dereferences must all exist there, or this TU does not compile.
 *
 * vms-unwind.h's fallback routine alpha_vms_fallback_frame_state is `static`;
 * the anchor below takes its address so cc1 must emit it, giving the proof a
 * real VMS/Alpha procedure descriptor (.ent/.pdesc) to grep for. */

#include "libgcc_unwind_harness.h"
#include "vms-unwind.h"

void *vms_unwind_proof_anchor(void)
{
  return (void *) &alpha_vms_fallback_frame_state;
}
