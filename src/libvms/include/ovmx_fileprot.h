/*
 * ovmx_fileprot.h - VMS file protection word (SOGW) encoding, PINNED.
 *
 * WHY THIS HEADER EXISTS (vms-f81, P1). Two OVMX modules independently
 * re-implemented the same 16-bit VMS file-protection-word layout and
 * DISAGREED with each other:
 *
 *   - src/vmsfs/vmsfs_protect.c (vmsfs_mode_to_protection /
 *     vmsfs_protection_to_mode) packed the four category nibbles at
 *     shifts SYSTEM=0 / OWNER=4 / GROUP=8 / WORLD=12, with per-nibble
 *     bit values R=0x01 / W=0x02 / E=0x04 / D=0x08.
 *   - src/libvms/syssvc/sys_security.c (sys$chkpro) read them at the
 *     MIRROR-IMAGE shifts SYSTEM=12 / OWNER=8 / GROUP=4 / WORLD=0, with
 *     per-nibble bit values R=0x08 / W=0x04 / E=0x02 / D=0x01 -- both the
 *     nibble order AND the intra-nibble bit order exactly reversed.
 *
 * A protection word built by one and read by the other silently checked
 * the WRONG category and the WRONG access bit. This header is the single
 * source both now include, so the two encodings cannot drift apart again.
 *
 * WHICH ENCODING IS CORRECT (CLAUDE.md Rule 8 -- ground it, don't pick):
 * vmsfs_protect.c's was. Confirmed against the VSI OpenVMS Wiki's $CRMPSC
 * page (https://wiki.vmssoftware.com/$CRMPSC, the PROT parameter for
 * $CRMPSC's protection mask, which is the same file/section-protection
 * mask format as RMS's FAB$W_PRO / XAB$W_PRO -- one mask, several system
 * services), fetched 2026-08-09:
 *
 *   "The mask contains four 4-bit fields. Bits are read from right to
 *    left in each field. Cleared bits indicate that read, write, execute,
 *    and delete access, in that order, are granted to the particular
 *    category of user."
 *
 * and the VSI wiki's own worked example of the OpenVMS DEFAULT file
 * protection mask (https://wiki.vmssoftware.com/RMS_FILEPROT and
 * https://wiki.vmssoftware.com/Default_Protection, both fetched
 * 2026-08-09): decimal 64000 / hex FA00 = "(S:RWED,O:RWED,G:RE,W:)".
 *
 * DECODING THAT EXAMPLE BIT-FOR-BIT is what actually pins the layout
 * (a textual description alone leaves the category order ambiguous; a
 * worked numeric example does not):
 *
 *   0xFA00 = 1111 1010 0000 0000
 *             ---- ---- ---- ----
 *             F    A    0    0
 *            b15-12 b11-8 b7-4 b3-0
 *
 *   The two all-grant (0000) nibbles must be System and Owner (both
 *   "RWED" = nothing denied) per the documented order System, Owner,
 *   Group, World (low category first, as VSI's UIC_Protection page and
 *   this same default-mask listing both name them, S first / W last).
 *   That forces System into bits 3-0 and Owner into bits 7-4 -- the two
 *   LOW nibbles, not the two high ones. The remaining nibbles must be
 *   Group (bits 11-8 = 0xA = 1010) and World (bits 15-12 = 0xF = 1111).
 *   World = 1111 (all denied) matches "W:" (nothing granted). Group =
 *   1010 must decode to "RE" (Read+Execute granted, Write+Delete
 *   denied): reading right-to-left per $CRMPSC's own wording, bit0=R,
 *   bit1=W, bit2=E, bit3=D --> 1010 has bit3(D)=1, bit2(E)=0, bit1(W)=1,
 *   bit0(R)=0, i.e. D denied, E granted, W denied, R granted = "RE".
 *   Matches exactly.
 *
 * Result: nibble order SYSTEM(bits 0-3) / OWNER(4-7) / GROUP(8-11) /
 * WORLD(12-15), and per-nibble bit order R(bit0=0x1) / W(bit1=0x2) /
 * E(bit2=0x4) / D(bit3=0x8), set bit = access DENIED. This is exactly
 * vmsfs_protect.c's pre-existing encoding, and exactly 0xFF00 as the
 * "S:RWED,O:RWED,G:,W:" all-groups-denied default protection value that
 * src/vmsrms/rms_core.c's rms_get_default_protection() already returns --
 * a THIRD independent internal consumer that only makes sense under this
 * encoding, corroborating it further.
 *
 * sys_security.c's sys$chkpro is the one that was wrong and is fixed to
 * this header's constants (vms-f81).
 *
 * NOT IN SCOPE / already handled elsewhere: rms_core.c used to carry a
 * THIRD, independent copy of these bit values (RMS_PROT_READ/WRITE/
 * EXECUTE/DELETE) for its own rms_check_protection() pre-check, which
 * also disagreed with vmsfs_protect.c. That whole decision point --
 * rms_check_protection() and its three call sites -- was DELETED (not
 * fixed) by vms-2b8 (operator ruling 2026-07-31): it was a userspace
 * second opinion that could only produce false denials, never enforce
 * anything the real open()/unlink() DAC check didn't already decide. See
 * sys_security.c's comment at the deletion site. There is no third
 * consumer left to pin here.
 */

#ifndef __OVMX_FILEPROT_H
#define __OVMX_FILEPROT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Per-category access bits (within a 4-bit nibble). Set = DENIED. */
#define VMS_PROT_R  0x01  /* Read */
#define VMS_PROT_W  0x02  /* Write */
#define VMS_PROT_E  0x04  /* Execute */
#define VMS_PROT_D  0x08  /* Delete */

/* Shift positions for each category in the 16-bit protection word. */
#define VMS_PROT_SYSTEM_SHIFT  0
#define VMS_PROT_OWNER_SHIFT   4
#define VMS_PROT_GROUP_SHIFT   8
#define VMS_PROT_WORLD_SHIFT   12

/* All access denied within a category (all 4 bits set). */
#define VMS_PROT_ALL_DENIED  0x0F

/* No access denied within a category (full access). */
#define VMS_PROT_ALL_ALLOWED 0x00

#ifdef __cplusplus
}
#endif

#endif /* __OVMX_FILEPROT_H */
