/*
 * prv_agreement.c - Agreement lock between the userspace privilege table
 *                   and the executive's copy (vms-2b8).
 *
 * vms.ko carries its own copy of the privilege bit positions (VMS_PRV_V_*
 * in src/kernel/vms_ioctl.h) because a kernel module cannot include
 * prvdef.h -- prvdef.h is a userspace header and pulls in <stdint.h>.
 * Two copies of a security-critical table is how this tree ended up with
 * FOUR disagreeing privilege tables, three of them wrong -- most damagingly
 * src/kernel/vms_access.c checking bit 5 (DETACH) and calling it SETPRV.
 *
 * THIS FILE EXISTS SO THAT A DIVERGENCE IS A BUILD FAILURE.
 *
 * The assertions were previously inside prvdef.h under `#ifdef _VMS_IOCTL_H`
 * -- i.e. they only compiled if some translation unit happened to include
 * the executive's header BEFORE prvdef.h. No translation unit in the tree
 * did, so the block never compiled and the guard enforced nothing while
 * announcing that it did. That is exactly the defect class vms-2b8 exists
 * to delete, so the guard now lives in a translation unit of its own,
 * unconditionally, with both headers included here by name. There is no
 * preprocessor condition left that can switch it off, and CMakeLists.txt
 * builds this file into LIBVMS$SHR as part of the default target.
 *
 * Negative control (run it if you touch either table): change PRV$V_WORLD
 * in src/libvms/include/prvdef.h to a wrong value and rebuild -- the build
 * MUST fail here. A guard whose negative control has not been run is not
 * a guard.
 *
 * Both tables are pinned to the reference lab OpenVMS VAX V7.3 node VAX1
 * via SDA READ SYS$SYSTEM:SYSDEF.STB; see docs/oracle/vax73-privileges.md
 * section 2 for the verbatim EVALUATE transcript.
 *
 * The check is confined to the privileges the two sides actually share --
 * the executive deliberately does not enumerate all 39, because it only
 * names privileges it can enforce or must store.
 */

#include "../kernel/vms_ioctl.h"
#include "prvdef.h"
#include "prv_names.h"

_Static_assert(PRV$V_CMKRNL == VMS_PRV_V_CMKRNL, "PRV$V_CMKRNL disagrees with the executive");
_Static_assert(PRV$V_CMEXEC == VMS_PRV_V_CMEXEC, "PRV$V_CMEXEC disagrees with the executive");
_Static_assert(PRV$V_DETACH == VMS_PRV_V_DETACH, "PRV$V_DETACH disagrees with the executive");
_Static_assert(PRV$V_LOG_IO == VMS_PRV_V_LOG_IO, "PRV$V_LOG_IO disagrees with the executive");
_Static_assert(PRV$V_GROUP  == VMS_PRV_V_GROUP,  "PRV$V_GROUP disagrees with the executive");
_Static_assert(PRV$V_PSWAPM == VMS_PRV_V_PSWAPM, "PRV$V_PSWAPM disagrees with the executive");
_Static_assert(PRV$V_SETPRI == VMS_PRV_V_SETPRI, "PRV$V_SETPRI disagrees with the executive");
_Static_assert(PRV$V_SETPRV == VMS_PRV_V_SETPRV, "PRV$V_SETPRV disagrees with the executive");
_Static_assert(PRV$V_TMPMBX == VMS_PRV_V_TMPMBX, "PRV$V_TMPMBX disagrees with the executive");
_Static_assert(PRV$V_WORLD  == VMS_PRV_V_WORLD,  "PRV$V_WORLD disagrees with the executive");
_Static_assert(PRV$V_OPER   == VMS_PRV_V_OPER,   "PRV$V_OPER disagrees with the executive");
_Static_assert(PRV$V_NETMBX == VMS_PRV_V_NETMBX, "PRV$V_NETMBX disagrees with the executive");
_Static_assert(PRV$V_SYSPRV == VMS_PRV_V_SYSPRV, "PRV$V_SYSPRV disagrees with the executive");
_Static_assert(PRV$V_BYPASS == VMS_PRV_V_BYPASS, "PRV$V_BYPASS disagrees with the executive");

/*
 * The masks the executive derives from those positions must agree too.
 * These are shifts of the VMS_PRV_V_* above, so they cannot diverge
 * independently -- but asserting them keeps the mask spellings honest if
 * someone ever hand-writes one.
 */
_Static_assert(PRV$M_CMKRNL == VMS_PRV_M_CMKRNL, "PRV$M_CMKRNL disagrees with the executive");
_Static_assert(PRV$M_CMEXEC == VMS_PRV_M_CMEXEC, "PRV$M_CMEXEC disagrees with the executive");
_Static_assert(PRV$M_SETPRV == VMS_PRV_M_SETPRV, "PRV$M_SETPRV disagrees with the executive");
_Static_assert(PRV$M_TMPMBX == VMS_PRV_M_TMPMBX, "PRV$M_TMPMBX disagrees with the executive");
_Static_assert(PRV$M_WORLD  == VMS_PRV_M_WORLD,  "PRV$M_WORLD disagrees with the executive");
_Static_assert(PRV$M_NETMBX == VMS_PRV_M_NETMBX, "PRV$M_NETMBX disagrees with the executive");

/*
 * F$GETJPI CURPRIV/AUTHPRIV NAME COVERAGE (vms-2b8 round 9; supersedes a
 * RUNTIME check rounds 7-8 put in src/vmsdcl/dcl_lexical.c's lex_getjpi()).
 * Round 10 widened WHAT this assert can catch -- read that part before
 * relying on either direction below.
 *
 * lex_getjpi() renders CURPRIV/AUTHPRIV by walking VMS_PRV_M_ENFORCED bit
 * by bit and looking each set bit up in vms_priv_names[] (dcl_cmd_show.c)
 * -- deliberately, so a bit added to VMS_PRV_M_ENFORCED needs no second,
 * hand-maintained name list (vms-2b8 round 6). Whether every bit
 * VMS_PRV_M_ENFORCED can set has a row in vms_priv_names[] is decided
 * entirely by two pieces of static, compile-time-constant data in the
 * SAME binary -- there is no caller, no runtime state and no execution
 * path that can make this vary. Rounds 7-8 nonetheless guarded it with a
 * RUNTIME check (walk the mask at F$GETJPI time; abort() if a bit has no
 * row) reached the moment a user ran F$GETJPI CURPRIV. That is Rule 10's
 * forbidden third answer: a plausible-looking handler for a condition
 * that is already settled before the program runs, not one that can
 * arise while it is running. The corrected HIDE answer for a fact fixed
 * at compile time is a compile-time proof, not a runtime abort -- so the
 * runtime guard is DELETED (dcl_lexical.c, round 9).
 *
 * ROUND 9's ASSERT CHECKED VMS_PRV_M_ENFORCED AGAINST A HAND-TYPED
 * WHITELIST, NOT AGAINST vms_priv_names[]'S ACTUAL CONTENT. Its body was
 * `VMS_PRV_M_ENFORCED & ~(PRV$M_CMKRNL | PRV$M_CMEXEC | PRV$M_SETPRV |
 * PRV$M_WORLD)`, four names typed by a person reading the array, not
 * derived from it. Round 9's commit message called this "structurally
 * impossible at compile time" for the coverage desync; that claim was
 * false in the direction that matters. The whitelist and the array were
 * two independent pieces of text that happened to agree -- deleting the
 * WORLD row from vms_priv_names[] (dcl_cmd_show.c) left the whitelist
 * untouched, so the build still succeeded with VMS_PRV_M_ENFORCED's WORLD
 * bit reaching lex_getjpi()'s lookup loop with no row: the exact desync
 * the assert claimed to prevent, arriving from the table side instead of
 * the mask side. PROVEN (round 10): deleting the WORLD row from the
 * round-9 tree left `ctest` and the full build green -- see the round-10
 * commit message for the exact command and output.
 *
 * ROUND 10 FIX: vms_priv_names[] (dcl_cmd_show.c) is no longer hand-typed
 * -- it is generated from VMS_PRIV_NAME_LIST (src/libvms/include/
 * prv_names.h), and VMS_PRIV_NAMES_TABLE_MASK below is the OR of every
 * mask term in that SAME list. Both are textually derived from one
 * preprocessor list; there is no second, independently-maintained copy
 * of "which rows exist" left for a row deletion to leave stale. Deleting
 * a line from VMS_PRIV_NAME_LIST removes that privilege's row from
 * vms_priv_names[] AND its mask term from VMS_PRIV_NAMES_TABLE_MASK in
 * the same edit, so a row deleted while VMS_PRV_M_ENFORCED still sets
 * that bit now fails THIS assert. This is what makes the check
 * structural rather than a copy that happens to agree today: read
 * prv_names.h's header comment for the mechanism, and see the two
 * negative controls below for both directions proven by execution
 * rather than asserted by comment.
 *
 * NEGATIVE CONTROL 1 -- bit added to VMS_PRV_M_ENFORCED with no table row
 * (same direction round 9 covered; re-run after the round-10 refactor to
 * confirm it survived): OR (1ULL << 40) into VMS_PRV_M_ENFORCED
 * (src/kernel/vms_ioctl.h) and rebuild -- the build MUST fail here.
 * RUN (vms-2b8 round 10): it does. Reverted after confirming.
 *
 * NEGATIVE CONTROL 2 -- row deleted from the table while VMS_PRV_M_ENFORCED
 * still sets that bit (the direction round 9's assert missed and round 10
 * exists to close): delete the `X(WORLD, PRV$M_WORLD, ...)` line from
 * VMS_PRIV_NAME_LIST (src/libvms/include/prv_names.h) and rebuild -- the
 * build MUST fail here. RUN (vms-2b8 round 10): it does. Reverted after
 * confirming.
 */
_Static_assert((VMS_PRV_M_ENFORCED & ~VMS_PRIV_NAMES_TABLE_MASK) == 0,
               "VMS_PRV_M_ENFORCED (src/kernel/vms_ioctl.h) sets a bit with no "
               "row in VMS_PRIV_NAME_LIST (src/libvms/include/prv_names.h, "
               "which also generates vms_priv_names[] in "
               "src/vmsdcl/dcl_cmd_show.c) -- add a row for it there before "
               "widening VMS_PRV_M_ENFORCED (vms-2b8)");

/*
 * A translation unit consisting only of static assertions produces an
 * object file with no symbols. Some archivers and some link steps treat
 * that as an empty member worth dropping; give the object one externally
 * visible datum so the file is unmistakably part of the library and so a
 * reader can confirm from `nm LIBVMS$SHR.EXE` that the lock was compiled.
 */
const char ovmx_prv_agreement_locked[] = "vms-2b8: privilege table agrees with the executive";
