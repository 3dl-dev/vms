/* libgcc_unwind_harness.h - unwinder-ABI prologue for the vms-unwind.h
 * include-surface proof (vms-8e8c, CHF rung-5).
 *
 * WHAT THIS IS (and is NOT):
 *   libgcc/config/alpha/vms-unwind.h is not a standalone translation unit - in
 *   a real libgcc build it is textually #included INTO libgcc/unwind-dw2.c as
 *   md-unwind-support.h, AFTER that file has defined `struct _Unwind_Context`,
 *   `_Unwind_FrameState`, the REG_SAVED_* / CFA_* enums and the _URC_* codes.
 *   Those types are libgcc's OWN internal unwinder ABI - they are NOT the OVMX
 *   VMS include surface and NOT the port's VMS types. This header reproduces
 *   just that unwinder ABI so the verbatim vms-unwind.h can be compiled on its
 *   own, exactly the scaffolding unwind-dw2.c would have supplied.
 *
 *   It deliberately does NOT define any `vms/...` type (pdscdef/chfdef/
 *   chfctxdef/libicb): those come ONLY from src/libvms/include's vms/ shim
 *   under test, so this proof isolates the OVMX include surface. The baseline
 *   control in run_include_surface_proof.sh compiles the SAME wrapper with the
 *   NEW shim file (vms/chfctxdef.h) removed and asserts it fails unresolved -
 *   this prologue is present in both runs, so the only variable is the shim.
 *
 * PROVENANCE (Rule 8): the struct/enum shapes below are the public libgcc
 * unwinder ABI, reproduced from GCC 14.2.0 (libgcc/unwind-dw2.h,
 * libgcc/unwind-dw2.c, libgcc/unwind-generic.h, libgcc/unwind-dw2-fde.h) -
 * the same GCC 14.2.0 sources vendored at tools/cross-alpha-vms/gcc-14.2.0.tar.xz.
 * Not VSI/HPE material.
 */
#ifndef LIBGCC_UNWIND_HARNESS_H
#define LIBGCC_UNWIND_HARNESS_H

/* Frame-register count + alt return column. Normally predefined by cc1 from the
 * Alpha backend (FIRST_PSEUDO_REGISTER == 64, DWARF_ALT_FRAME_RETURN_COLUMN ==
 * 64); guarded so the compiler's own values win when it supplies them. */
#ifndef __LIBGCC_DWARF_FRAME_REGISTERS__
#define __LIBGCC_DWARF_FRAME_REGISTERS__ 64
#endif
#ifndef __LIBGCC_DWARF_ALT_FRAME_RETURN_COLUMN__
#define __LIBGCC_DWARF_ALT_FRAME_RETURN_COLUMN__ 64
#endif

/* Fundamental unwind word types (libgcc/unwind-generic.h). 64-bit on Alpha. */
typedef unsigned long long _Unwind_Word;
typedef long long          _Unwind_Sword;
typedef void              *_Unwind_Personality_Fn;

/* _Unwind_Reason_Code (libgcc/unwind-generic.h) - only the members vms-unwind.h
 * returns matter, but the full enum is reproduced for fidelity. */
typedef enum
{
  _URC_NO_REASON = 0,
  _URC_FOREIGN_EXCEPTION_CAUGHT = 1,
  _URC_FATAL_PHASE2_ERROR = 2,
  _URC_FATAL_PHASE1_ERROR = 3,
  _URC_NORMAL_STOP = 4,
  _URC_END_OF_STACK = 5,
  _URC_HANDLER_FOUND = 6,
  _URC_INSTALL_CONTEXT = 7,
  _URC_CONTINUE_UNWIND = 8
} _Unwind_Reason_Code;

/* dwarf_eh_bases (libgcc/unwind-dw2-fde.h) - embedded by value in the context. */
struct dwarf_eh_bases
{
  void *tbase;
  void *dbase;
  void *func;
};

/* Register save-state kinds (libgcc/unwind-dw2.h). vms-unwind.h uses
 * REG_SAVED_OFFSET and REG_SAVED_REG. */
enum {
  REG_UNSAVED,
  REG_SAVED_OFFSET,
  REG_SAVED_REG,
  REG_SAVED_EXP,
  REG_SAVED_VAL_OFFSET,
  REG_SAVED_VAL_EXP,
  REG_UNSAVED_ARCHEXT,
  REG_UNDEFINED
};

/* _Unwind_Context (libgcc/unwind-dw2.c). vms-unwind.h reads reg[], cfa and ra;
 * the remaining members are reproduced so the type is the genuine shape. The
 * default (non-REG_VALUE_IN_UNWIND_CONTEXT) register cell is a void*. */
typedef void *_Unwind_Context_Reg_Val;

struct _Unwind_Context
{
  _Unwind_Context_Reg_Val reg[__LIBGCC_DWARF_FRAME_REGISTERS__+1];
  void *cfa;
  void *ra;
  void *lsda;
  struct dwarf_eh_bases bases;
  _Unwind_Word flags;
  _Unwind_Word version;
  _Unwind_Word args_size;
  char by_value[__LIBGCC_DWARF_FRAME_REGISTERS__+1];
};

/* _Unwind_FrameState (libgcc/unwind-dw2.h), reproduced verbatim. vms-unwind.h
 * writes regs.how[], regs.reg[].loc.offset / .loc.reg, regs.cfa_how/cfa_reg/
 * cfa_offset, retaddr_column and signal_frame. */
typedef struct
{
  struct frame_state_reg_info
  {
    struct {
      union {
	_Unwind_Word reg;
	_Unwind_Sword offset;
	const unsigned char *exp;
      } loc;
    } reg[__LIBGCC_DWARF_FRAME_REGISTERS__+1];
    unsigned char how[__LIBGCC_DWARF_FRAME_REGISTERS__+1];

    enum {
      CFA_UNSET,
      CFA_REG_OFFSET,
      CFA_EXP
    } cfa_how : 8;

    struct frame_state_reg_info *prev;

    _Unwind_Sword cfa_offset;
    _Unwind_Word cfa_reg;
    const unsigned char *cfa_exp;
  } regs;

  void *pc;
  _Unwind_Personality_Fn personality;
  _Unwind_Sword data_align;
  _Unwind_Word code_align;
  _Unwind_Word retaddr_column;
  unsigned char fde_encoding;
  unsigned char lsda_encoding;
  unsigned char saw_z;
  unsigned char signal_frame;
  void *eh_ptr;
} _Unwind_FrameState;

#endif /* LIBGCC_UNWIND_HARNESS_H */
