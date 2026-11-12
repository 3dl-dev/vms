/* OVMX (vms-ec70, vms-59a): shim for DEC C's <builtins.h>.
 *
 * MMK's queue macros (mmk.h queue_insert/queue_remove) take the Alpha branch
 * when the compiler defines __ALPHA (the alpha-dec-vms port compiler does) and
 * call the PALcode queue built-ins there; elsewhere they call _INSQUE/_REMQUE.
 * DEC C expands __PAL_INSQUEL/__PAL_REMQUEL inline to CALL_PAL; OVMX/Alpha runs
 * on the host kernel's PALcode, which has no VMS queue PAL calls, so they are
 * ordinary functions here (ovmx_mmk_builtins.c), with the documented operands
 * and results.  Nothing else in <builtins.h> is used by MMK.
 */
#ifndef OVMX_SHIM_BUILTINS_H
#define OVMX_SHIM_BUILTINS_H
#if defined(__ALPHA) || defined(__alpha)
int __PAL_INSQUEL(void *__predecessor, void *__new_entry);
int __PAL_REMQUEL(void *__entry, void *__removed_entry);
#endif
#endif
