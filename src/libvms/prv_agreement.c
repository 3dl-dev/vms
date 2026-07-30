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
 * A translation unit consisting only of static assertions produces an
 * object file with no symbols. Some archivers and some link steps treat
 * that as an empty member worth dropping; give the object one externally
 * visible datum so the file is unmistakably part of the library and so a
 * reader can confirm from `nm LIBVMS$SHR.EXE` that the lock was compiled.
 */
const char ovmx_prv_agreement_locked[] = "vms-2b8: privilege table agrees with the executive";
