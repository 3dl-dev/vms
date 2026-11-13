/*
 * ovmx_vmsabi_names.h - a 32-bit-pointer DEC C program calls the VMS-ABI
 * entry points by their VMS names (vms-7b3).
 *
 * DEC C upper-cases external names by default (/NAMES=UPPERCASE), so a VMS
 * program that writes str$prefix(&d, &s) calls STR$PREFIX, which takes the
 * VMS descriptor: the 8-byte 32-bit form, or the 64-bit form recognised by its
 * MBO/MBMO fields (src/libvms/syssvc/vmsabi_dsc.h). The alpha-dec-vms
 * compiler emits a name as written, so the same source would bind OVMX's
 * lower-case entries, which are OVMX's own LP64 routines: they read
 * dsc$a_pointer at offset 8 of a 16-byte descriptor and misread the 8-byte one
 * (MMK freed the longword after its descriptor, vms-4ac2 rung 2).
 *
 * In a unit the alpha-dec-vms compiler builds at the DEC C default 32-bit
 * pointer size -- the one case in which <descrip.h> and <starlet.h> lay their
 * structures out as VMS does -- each routine below takes its upper-case
 * external name, the effect /NAMES=UPPERCASE has for it. OVMX's own LP64 code
 * (-mpointer-size=64) and the host builds keep the lower-case entries.
 *
 * A routine is listed only when its VMS-ABI entry exists (an upper-case
 * definition in src/libvms/syssvc/sys_vmsabi.c, checked by
 * tests/libvms/check_vmsabi_names.sh); one without it keeps its lower-case
 * name until it has one (the vms-7b3 child items). The RMS services
 * (SYS$OPEN, SYS$GET, ...) are not listed yet: their VMS-ABI entries take the
 * VMS-layout FAB/RAB of <vms/rms.h>, not the <rms.h> layout a program built
 * against OVMX's headers fills in.
 */
#ifndef OVMX_VMSABI_NAMES_H
#define OVMX_VMSABI_NAMES_H

#if defined(__VMS) && defined(__alpha) && defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4

/* LIBRTL: LIB$ */
#pragma redefine_extname lib$free_vm      LIB$FREE_VM
#pragma redefine_extname lib$get_foreign  LIB$GET_FOREIGN
#pragma redefine_extname lib$get_symbol   LIB$GET_SYMBOL
#pragma redefine_extname lib$get_vm       LIB$GET_VM
#pragma redefine_extname lib$getdvi       LIB$GETDVI
#pragma redefine_extname lib$put_output   LIB$PUT_OUTPUT
#pragma redefine_extname lib$set_symbol   LIB$SET_SYMBOL
#pragma redefine_extname lib$sys_fao      LIB$SYS_FAO

/* LIBRTL: STR$ */
#pragma redefine_extname str$append       STR$APPEND
#pragma redefine_extname str$compare      STR$COMPARE
#pragma redefine_extname str$concat       STR$CONCAT
#pragma redefine_extname str$copy_dx      STR$COPY_DX
#pragma redefine_extname str$free1_dx     STR$FREE1_DX
#pragma redefine_extname str$position     STR$POSITION
#pragma redefine_extname str$prefix       STR$PREFIX

/* System services (SYS$PUBLIC_VECTORS) */
#pragma redefine_extname sys$assign       SYS$ASSIGN
#pragma redefine_extname sys$crembx       SYS$CREMBX
#pragma redefine_extname sys$dassgn       SYS$DASSGN
#pragma redefine_extname sys$delprc       SYS$DELPRC
#pragma redefine_extname sys$fao          SYS$FAO
#pragma redefine_extname sys$forcex       SYS$FORCEX
#pragma redefine_extname sys$getsyi       SYS$GETSYI
#pragma redefine_extname sys$qiow         SYS$QIOW
#pragma redefine_extname sys$trnlnm       SYS$TRNLNM
#pragma redefine_extname sys$wake         SYS$WAKE

#endif

#endif /* OVMX_VMSABI_NAMES_H */
