/*
 * test_conformance_layout.c — vms-801.5: VMS ABI struct LAYOUT-stability gate.
 *
 * WHAT THIS IS (and is NOT).  This is NOT a "binary compatibility with real
 * OpenVMS Alpha" audit.  That was the original vms-801.5 framing (written
 * 2026-07-25, pre-ODS-2-flip), and it is a FALSE premise for today's
 * architecture: the VMS ABI structs below (FAB/RAB/NAM, the dsc$descriptor_*
 * family, ILE3/ILE2) never cross a binary boundary.  Their producer
 * (src/vmsrms/crtl_rms_stdio.c, the DECC$SHR veneer) and their consumer
 * (sys$create/$open/$connect/... in LIBVMSRMS$SHR) are BOTH OVMX code, compiled
 * by the same toolchain from these same headers.  The alpha-dec-vms GCC port's
 * produced code touches ZERO of these structs — it uses only the C stdio ABI
 * (void*, char*, scalars), which cc1 decorates to decc$fopen/fwrite/...  So
 * OVMX deliberately uses NATIVE LP64 8-byte pointers in these structs, NOT the
 * 32-bit LONGWORD address fields of a real OpenVMS FAB/item-list.  That
 * divergence is correct precisely BECAUSE nothing shares the layout across a
 * boundary.  Asserting OVMX's layout against real-VMS byte offsets would be a
 * LARP (fake: OVMX-vs-OVMX can't fail) or a regression (wrong: forcing 32-bit
 * fields for a boundary that does not exist, breaking working LP64 code).
 *
 * WHAT THIS GATE ACTUALLY DOES:
 *
 *  1. LAYOUT-STABILITY REGRESSION LOCK.  Pins sizeof + representative offsets
 *     for each struct to values DERIVED INDEPENDENTLY from the documented VMS
 *     field order and the LP64 C-ABI alignment rules (each derivation is shown
 *     in the comments below — the expected values are NOT read back from
 *     offsetof and pasted here, which is what would make the gate vacuous).
 *     vms-801.6's RTL (CLI$/LIB$TPARSE/SYS$FILESCAN/LIB$FIND_FILE) consumes
 *     ILE3/ILE2 + descriptors; this catches a future field reorder/insert
 *     BEFORE it silently breaks that layer.
 *
 *  2. PUBLIC-PREFIX STABILITY.  OVMX-internal implementation fields
 *     (FAB._rms_file/_resolved_path/_rms_state, RAB._current_offset/...) must
 *     stay APPENDED after every public VMS field, so adding internal state
 *     never shifts a public offset.
 *
 *  3. LP64-DIVERGENCE TRIPWIRE (preserves the original binary-compat concern as
 *     an asserted INV-6 invariant).  Asserts the address fields ARE 8 bytes and
 *     names the boundary assumption: these structs are OVMX-internal and LP64.
 *     IF the GCC endgame ever loads precompiled real-VMS objects that build
 *     their own FAB/item-list and call in (the only place a genuine binary
 *     boundary could appear), this assumption breaks and RMS must grow a
 *     separate 32-bit-longword FAB64/RAB64/real-VMS-layout interface at that
 *     seam.  The tripwire makes that a loud, deliberate decision, not silent
 *     corruption.
 *
 * NON-FAKENESS PROOF (conductor non-negotiable): the companion runner
 * tests/libvms/run_struct_layout_gate.sh compiles this file a second time with
 * -DLAYOUT_GATE_INJECT_DRIFT, which introduces a deliberately-drifted layout;
 * that compile MUST fail.  A gate that cannot fail is fake — this one demonstrably
 * rejects a drifted layout.  See the LAYOUT_GATE_INJECT_DRIFT block at the end.
 *
 * Pure header test (no runtime dependency); needs the libvms and vmsrms include
 * dirs.  See tests/libvms/CMakeLists.txt.
 */

#include <stddef.h>   /* offsetof */
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h> /* off_t (used inside struct RAB) */

#include <descrip.h>
#include <iledef.h>
#include "rms/fab.h"
#include "rms/rab.h"
#include "rms/nam.h"

/* LP64 preconditions every derivation below assumes.  If either fails, this
 * host is not the LP64 model these structs are laid out for, and the pinned
 * offsets do not apply. */
_Static_assert(sizeof(void *) == 8, "layout gate assumes LP64 (8-byte pointers)");
_Static_assert(sizeof(off_t) == 8, "layout gate assumes 8-byte off_t (LP64)");

/* ================================================================
 * dsc$descriptor_s  (CLASS_S — the RTL/system-service workhorse)
 * Fields: uint16 length; uint8 dtype; uint8 class; char *pointer;
 * LP64 derivation:
 *   length  @0 (w)   dtype @2 (b)   class @3 (b)
 *   pointer @8 (8-aligned; +4 pad after class)     size = 16
 * dsc$descriptor_d and dsc$descriptor_vs have the identical field set.
 * ================================================================ */
#define EXP_DSC_LENGTH   0
#define EXP_DSC_DTYPE    2
#define EXP_DSC_CLASS    3
#define EXP_DSC_POINTER  8
#define EXP_DSC_SIZE     16

_Static_assert(offsetof(struct dsc$descriptor_s, dsc$w_length)  == EXP_DSC_LENGTH,  "dsc_s length");
_Static_assert(offsetof(struct dsc$descriptor_s, dsc$b_dtype)   == EXP_DSC_DTYPE,   "dsc_s dtype");
_Static_assert(offsetof(struct dsc$descriptor_s, dsc$b_class)   == EXP_DSC_CLASS,   "dsc_s class");
_Static_assert(offsetof(struct dsc$descriptor_s, dsc$a_pointer) == EXP_DSC_POINTER, "dsc_s pointer");
_Static_assert(sizeof(struct dsc$descriptor_s) == EXP_DSC_SIZE, "dsc_s size");
_Static_assert(sizeof(((struct dsc$descriptor_s *)0)->dsc$a_pointer) == 8,
    "LP64-divergence tripwire: dsc$a_pointer is a native 8-byte pointer, NOT a "
    "real-VMS 32-bit longword. Valid only while descriptors stay OVMX-internal.");

_Static_assert(offsetof(struct dsc$descriptor_d, dsc$a_pointer) == EXP_DSC_POINTER, "dsc_d pointer");
_Static_assert(sizeof(struct dsc$descriptor_d) == EXP_DSC_SIZE, "dsc_d size");
_Static_assert(offsetof(struct dsc$descriptor_vs, dsc$a_pointer) == EXP_DSC_POINTER, "dsc_vs pointer");
_Static_assert(sizeof(struct dsc$descriptor_vs) == EXP_DSC_SIZE, "dsc_vs size");

/* ================================================================
 * ILE3 — item list entry, 3-field (SYS$GETJPI/GETSYI/... input lists)
 * Fields: uint16 length; uint16 code; void *bufaddr; uint16 *retlen_addr;
 * LP64 derivation:
 *   length @0 (w)  code @2 (w)  bufaddr @8 (8-aligned; +4 pad)
 *   retlen_addr @16                                   size = 24
 * NB: iledef.h's own comment historically read "16 bytes on 64-bit" — that is
 * arithmetically wrong for LP64 (it is 24).  This gate pins the correct value;
 * the comment is corrected in the same change.
 * ================================================================ */
#define EXP_ILE3_LENGTH   0
#define EXP_ILE3_CODE     2
#define EXP_ILE3_BUFADDR  8
#define EXP_ILE3_RETLEN   16
#define EXP_ILE3_SIZE     24

_Static_assert(offsetof(ILE3, ile3$w_length)       == EXP_ILE3_LENGTH,  "ile3 length");
_Static_assert(offsetof(ILE3, ile3$w_code)         == EXP_ILE3_CODE,    "ile3 code");
_Static_assert(offsetof(ILE3, ile3$ps_bufaddr)     == EXP_ILE3_BUFADDR, "ile3 bufaddr");
_Static_assert(offsetof(ILE3, ile3$ps_retlen_addr) == EXP_ILE3_RETLEN,  "ile3 retlen");
_Static_assert(sizeof(ILE3) == EXP_ILE3_SIZE, "ile3 size (24 on LP64, NOT 16)");

/* ILE2 — 2-field (SYS$FILESCAN FSCN$_ output list)
 * Fields: uint16 length; uint16 code; void *bufaddr;
 *   length @0  code @2  bufaddr @8            size = 16 */
#define EXP_ILE2_BUFADDR  8
#define EXP_ILE2_SIZE     16
_Static_assert(offsetof(ILE2, ile2$ps_bufaddr) == EXP_ILE2_BUFADDR, "ile2 bufaddr");
_Static_assert(sizeof(ILE2) == EXP_ILE2_SIZE, "ile2 size");

/* ================================================================
 * struct FAB — File Access Block
 * LP64 derivation (public VMS fields, then OVMX-internal tail):
 *   bid@0 bln@1 ifi@2(w) fop@4(l) sts@8 stv@12 alq@16 deq@20(w)
 *   fac@22 shr@23 org@24 rat@25 rfm@26 journal@27 mrs@28(w)
 *   mrn@32(l; +2 pad) fna@40(8-aligned;+4 pad) fns@48
 *   dna@56(8-aligned;+7 pad) dns@64 nam@72(8-aligned;+7 pad) xab@80
 *   fsz@88                                    <-- LAST PUBLIC field
 *   _rms_file@96(8-aligned;+7 pad)            <-- FIRST INTERNAL field
 *   _resolved_path[1024]@104  _rms_state@1128    size = 1136
 * ================================================================ */
#define EXP_FAB_FOP       4
#define EXP_FAB_MRS       28
#define EXP_FAB_FNA       40
#define EXP_FAB_NAM       72
#define EXP_FAB_XAB       80
#define EXP_FAB_FSZ       88   /* last public field */
#define EXP_FAB_RMS_FILE  96   /* first internal field */
#define EXP_FAB_SIZE      1136

_Static_assert(offsetof(struct FAB, fab$l_fop) == EXP_FAB_FOP, "fab fop");
_Static_assert(offsetof(struct FAB, fab$w_mrs) == EXP_FAB_MRS, "fab mrs");
_Static_assert(offsetof(struct FAB, fab$l_fna) == EXP_FAB_FNA, "fab fna");
_Static_assert(offsetof(struct FAB, fab$l_nam) == EXP_FAB_NAM, "fab nam");
_Static_assert(offsetof(struct FAB, fab$l_xab) == EXP_FAB_XAB, "fab xab");
_Static_assert(offsetof(struct FAB, fab$b_fsz) == EXP_FAB_FSZ, "fab fsz (last public)");
_Static_assert(offsetof(struct FAB, _rms_file) == EXP_FAB_RMS_FILE, "fab _rms_file (first internal)");
_Static_assert(sizeof(struct FAB) == EXP_FAB_SIZE, "fab size");
/* Public-prefix stability: every internal field is past the last public one. */
_Static_assert(offsetof(struct FAB, _rms_file)  > offsetof(struct FAB, fab$b_fsz), "fab internal after public");
_Static_assert(offsetof(struct FAB, _rms_state) > offsetof(struct FAB, _rms_file), "fab _rms_state after _rms_file");
_Static_assert(sizeof(((struct FAB *)0)->fab$l_fna) == 8,
    "LP64-divergence tripwire: fab$l_fna is a native 8-byte pointer, NOT a "
    "real-VMS 32-bit longword. See file header.");

/* ================================================================
 * struct RAB — Record Access Block
 * LP64 derivation:
 *   bid@0 bln@1 isi@2(w) sts@4 stv@8 rop@12 fab@16(ptr) ubf@24
 *   usz@32(w) rbf@40(8-aligned;+6 pad) rsz@48(w) rac@50 krf@51
 *   kbf@56(8-aligned;+4 pad) ksz@64 bkt@68(l;+3 pad) rfa@72(RFA=3*w=6B)
 *   ctx@80(8-aligned;+2 pad)                  <-- LAST PUBLIC field
 *   _current_offset@88(off_t)                 <-- FIRST INTERNAL field
 *   _rms_stream@96 _eof@104 _last_rec_offset@112 _last_rec_size@120(w)
 *   _rec_lock_lkid@124(l)                        size = 128
 * ================================================================ */
#define EXP_RAB_FAB       16
#define EXP_RAB_KBF       56
#define EXP_RAB_CTX       80   /* last public field */
#define EXP_RAB_CUROFF    88   /* first internal field */
#define EXP_RAB_SIZE      128

_Static_assert(offsetof(struct RAB, rab$l_fab) == EXP_RAB_FAB, "rab fab");
_Static_assert(offsetof(struct RAB, rab$l_kbf) == EXP_RAB_KBF, "rab kbf");
_Static_assert(offsetof(struct RAB, rab$l_ctx) == EXP_RAB_CTX, "rab ctx (last public)");
_Static_assert(offsetof(struct RAB, _current_offset) == EXP_RAB_CUROFF, "rab _current_offset (first internal)");
_Static_assert(sizeof(struct RAB) == EXP_RAB_SIZE, "rab size");
_Static_assert(offsetof(struct RAB, _current_offset) > offsetof(struct RAB, rab$l_ctx), "rab internal after public");
_Static_assert(sizeof(((struct RAB *)0)->rab$l_ubf) == 8,
    "LP64-divergence tripwire: rab$l_ubf is a native 8-byte pointer. See file header.");

/* ================================================================
 * struct NAM — Name Block
 * LP64 derivation (pre-existing fields, then the vms-ec70 appended tail):
 *   bid@0 bln@1 sts@4(l;+2 pad) esa@8(ptr) ess@16 esl@17
 *   rsa@24(8-aligned;+6 pad) rss@32 rsl@33 node..ver length bytes @34..39
 *   node@40 dev@48 dir@56 name@64 type@72 ver@80 (component ptrs)
 *   fnb@88(l) wcc@92(l) $$context@96(internal ptr)
 *   nop@104  rlf@112(8-aligned;+7 pad)  dvi[16]@120    size = 136
 * The vms-ec70 fields (nop/rlf/dvi) were "appended at the end so all
 * pre-existing field offsets are unchanged"; pinning both the pre-existing
 * offsets (esa/name/fnb) AND the appended ones enforces that promise.
 * ================================================================ */
#define EXP_NAM_ESA       8
#define EXP_NAM_NAME      64
#define EXP_NAM_FNB       88
#define EXP_NAM_NOP       104  /* first vms-ec70 appended field */
#define EXP_NAM_RLF       112
#define EXP_NAM_DVI       120
#define EXP_NAM_SIZE      136

_Static_assert(offsetof(struct NAM, nam$l_esa)  == EXP_NAM_ESA,  "nam esa");
_Static_assert(offsetof(struct NAM, nam$l_name) == EXP_NAM_NAME, "nam name");
_Static_assert(offsetof(struct NAM, nam$l_fnb)  == EXP_NAM_FNB,  "nam fnb");
_Static_assert(offsetof(struct NAM, nam$b_nop)  == EXP_NAM_NOP,  "nam nop (vms-ec70 appended)");
_Static_assert(offsetof(struct NAM, nam$l_rlf)  == EXP_NAM_RLF,  "nam rlf (vms-ec70 appended)");
_Static_assert(offsetof(struct NAM, nam$t_dvi)  == EXP_NAM_DVI,  "nam dvi (vms-ec70 appended)");
_Static_assert(sizeof(struct NAM) == EXP_NAM_SIZE, "nam size");
_Static_assert(sizeof(((struct NAM *)0)->nam$l_esa) == 8,
    "LP64-divergence tripwire: nam$l_esa is a native 8-byte pointer. See file header.");

/* ================================================================
 * NON-FAKENESS PROOF — deliberately-drifted layout.
 *
 * Compiled ONLY under -DLAYOUT_GATE_INJECT_DRIFT (by run_struct_layout_gate.sh).
 * ILE3_drifted inserts a pointer field ahead of the buffer address, shifting
 * every following offset.  Feeding it through the SAME pins the real ILE3
 * satisfies MUST fail to compile — proving the pins genuinely reject a drifted
 * layout rather than tautologically matching whatever the header happens to be.
 * A pointer (not a byte) is inserted so the drift is guaranteed to move the
 * 8-aligned bufaddr rather than vanish into alignment padding.
 * ================================================================ */
#ifdef LAYOUT_GATE_INJECT_DRIFT
struct ILE3_drifted {
    uint16_t  ile3$w_length;
    uint16_t  ile3$w_code;
    void     *DRIFT_injected;        /* the deliberate drift */
    void     *ile3$ps_bufaddr;
    uint16_t *ile3$ps_retlen_addr;
};
_Static_assert(offsetof(struct ILE3_drifted, ile3$ps_bufaddr) == EXP_ILE3_BUFADDR
               && sizeof(struct ILE3_drifted) == EXP_ILE3_SIZE,
    "DRIFT PROOF: this assertion is EXPECTED to fail — it proves the layout gate "
    "catches a shifted struct. If this compiles, the gate is fake.");
#endif

int main(void)
{
    /* Diagnostic dump — the assertions above already enforce the gate at
     * compile time; a successful build IS the pass.  This prints the pinned
     * layout so a run log shows the audited surface. */
    printf("VMS ABI struct layout audit (LP64) — vms-801.5\n");
    printf("  dsc$descriptor_s : size=%zu pointer@%zu\n",
           sizeof(struct dsc$descriptor_s), offsetof(struct dsc$descriptor_s, dsc$a_pointer));
    printf("  ILE3             : size=%zu bufaddr@%zu retlen@%zu\n",
           sizeof(ILE3), offsetof(ILE3, ile3$ps_bufaddr), offsetof(ILE3, ile3$ps_retlen_addr));
    printf("  ILE2             : size=%zu bufaddr@%zu\n",
           sizeof(ILE2), offsetof(ILE2, ile2$ps_bufaddr));
    printf("  struct FAB       : size=%zu fsz(last-pub)@%zu _rms_file(1st-int)@%zu\n",
           sizeof(struct FAB), offsetof(struct FAB, fab$b_fsz), offsetof(struct FAB, _rms_file));
    printf("  struct RAB       : size=%zu ctx(last-pub)@%zu _current_offset(1st-int)@%zu\n",
           sizeof(struct RAB), offsetof(struct RAB, rab$l_ctx), offsetof(struct RAB, _current_offset));
    printf("  struct NAM       : size=%zu esa@%zu nop(ec70)@%zu\n",
           sizeof(struct NAM), offsetof(struct NAM, nam$l_esa), offsetof(struct NAM, nam$b_nop));
    printf("ALL VMS ABI STRUCT LAYOUT ASSERTIONS PASSED (compile-time)\n");
    return 0;
}
