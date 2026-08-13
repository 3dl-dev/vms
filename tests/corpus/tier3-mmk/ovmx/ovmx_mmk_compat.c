/*
 * ovmx_mmk_compat.c - OVMX (vms-ec70) RTL call-arity forwarding wrappers for
 *                     the vendored MadGoat MMK (self-host spine #4).
 *
 * *** OVMX DESIGN CHOICE (clean-room, Rule 8) ***
 *
 * MMK calls several VMS routines omitting optional trailing arguments (AST
 * addresses, item lists) that OVMX declares with fixed prototypes.  The
 * force-included ovmx_mmk_compat.h maps each such name to one of the variadic
 * wrappers below; every wrapper simply supplies the omitted arguments and
 * forwards to the REAL OVMX routine.  No OVMX routine is weakened.
 *
 * This file may be compiled WITH the force-included ovmx_mmk_compat.h (uniform
 * build flags); the #undef block below drops the wrapper macros so the bodies
 * here call the REAL OVMX routines instead of recursing into themselves.
 */
#include <stdint.h>
#include <stdarg.h>

/* Drop the call-site wrapper macros so this TU sees the real OVMX routines. */
#undef sys$parse
#undef sys$search
#undef sys$filescan
#undef lib$getdvi
#undef ots$cvt_tu_l
#undef str$position
#undef lib$get_symbol
#undef lib$set_logical
#undef lib$create_vm_zone
#include <descrip.h>
#include <ssdef.h>
#include <rms.h>
#include <starlet.h>
#include <lib$routines.h>
#include <str$routines.h>
#include <ots$routines.h>
#include <fscndef.h>

/* $PARSE / $SEARCH: MMK passes (fab) or (fab,0,0); OVMX takes (fab,err,suc). */
uint32_t ovmx_mmk_sys_parse(void *fab, ...)  { return sys$parse(fab, 0, 0); }
uint32_t ovmx_mmk_sys_search(void *fab, ...) { return sys$search(fab, 0, 0); }

/* $FILESCAN: MMK passes (src,list,flags) or (src,list,flags,0,0); OVMX takes 3. */
uint32_t ovmx_mmk_sys_filescan(const void *srcstr, void *valuelst, void *fldflags, ...)
{
    return sys$filescan((const struct dsc$descriptor_s *)srcstr,
                        (ILE2 *)valuelst, (uint32_t *)fldflags);
}

/* LIB$GETDVI: MMK's older form (item, &chan, devnam, &result); OVMX's full form
 * is (item, chan-by-value, devnam, resultval, resultstring, string_length).
 * MMK uses this only in its terminal CTRL-T code (misc.c), off the spine path;
 * forward best-effort with the string outputs omitted. */
uint32_t ovmx_mmk_lib_getdvi(const void *item_code, void *chan, void *devnam,
                             void *result, ...)
{
    uint16_t ch = chan ? *(uint16_t *)chan : 0;
    return lib$getdvi((const uint32_t *)item_code, ch,
                      (const struct dsc$descriptor_s *)devnam, result, 0, 0);
}

/* OTS$CVT_TU_L: MMK passes (src,dest); OVMX takes (src,dest,size,flags). */
uint32_t ovmx_mmk_ots_cvt_tu_l(const void *src, void *dest, ...)
{
    return ots$cvt_tu_l((const struct dsc$descriptor_s *)src,
                        (uint32_t *)dest, sizeof(uint32_t), 0);
}

/* STR$POSITION: MMK passes (src,sub) or (src,sub,&start); OVMX takes 3. */
uint32_t ovmx_mmk_str_position(const void *src, const void *sub, ...)
{
    va_list ap; const uint32_t *start = 0;
    va_start(ap, sub); start = va_arg(ap, const uint32_t *); va_end(ap);
    return str$position((const struct dsc$descriptor_s *)src,
                        (const struct dsc$descriptor_s *)sub, start);
}

/* LIB$GET_SYMBOL: MMK passes (sym,val); OVMX takes (sym,val,vallen,tabtype). */
uint32_t ovmx_mmk_lib_get_symbol(const void *sym, void *val, ...)
{
    return lib$get_symbol((const struct dsc$descriptor_s *)sym,
                          (struct dsc$descriptor_s *)val, 0, 0);
}

/* LIB$CREATE_VM_ZONE: MMK passes (zone_id, &algorithm, &blocksize, &flags) —
 * 3 optional args.  OVMX's lib$create_vm_zone unconditionally va_arg's TEN
 * optional arguments (to reach the position-10 zone-name), which reads PAST the
 * caller's actual argument list on the SysV x86-64 ABI (there is no VMS
 * argument-count register) and then dereferences garbage as a name descriptor.
 * Forward with an explicit, fully-populated NULL argument list so the name slot
 * is a real NULL — no over-read, no name.  OVMX zones are quick-fit and ignore
 * the algorithm/block-size hints, so dropping them is functionally identical. */
uint32_t ovmx_mmk_create_vm_zone(uint32_t *zone_id, ...)
{
    return lib$create_vm_zone(zone_id,
        (void*)0,(void*)0,(void*)0,(void*)0,(void*)0,
        (void*)0,(void*)0,(void*)0,(void*)0,(void*)0);
}

/* LIB$SET_LOGICAL: MMK passes (lognam,eqvnam); OVMX takes 5. */
uint32_t ovmx_mmk_lib_set_logical(const void *lognam, const void *eqvnam, ...)
{
    return lib$set_logical((const struct dsc$descriptor_s *)lognam,
                           (const struct dsc$descriptor_s *)eqvnam, 0, 0, 0);
}
