/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ovmx_sshd_session.c - OVMX OpenSSH sshd SESSION drop seam (rd vms-0cd, RUNG-3
 * step 3c, design §B). The `--wrap=permanently_set_uid` half of the session shim
 * (the companion `--wrap=execve` lives in ovmx_sshd_exec.c). Unmodified OpenSSH;
 * OVMX enters via linker --wrap only.
 *
 *   __wrap_permanently_set_uid(pw): while STILL ROOT, run the LOGINOUT pre-drop
 *       (ovmx_sshd_pre_drop_pw, src/vmsssh/sshd_session.c) -- executive setident
 *       fail-closed (%OVMX-F-NOIDENT+_exit), banner, accounting -- then OpenSSH's
 *       own permanently_set_uid performs the ordered, permanent, fail-closed drop
 *       to the SYSUAF UIC (so cred_drop.c is not re-linked here). permanently_
 *       set_uid is OpenSSH's own symbol; --wrap redirects every caller to us and
 *       renames the original __real_permanently_set_uid.
 *
 * WHY THIS WRAP IS IN ITS OWN TU (separate from __wrap_execve): permanently_set_
 * uid is defined in uidswap.o, which only the session-running binaries
 * (sshd-session/sshd-auth) link -- NOT the sshd listener. The adapters are linked
 * as an ARCHIVE (on-demand), so this object is pulled only where permanently_set_
 * uid is actually referenced, keeping the unresolved __real_permanently_set_uid
 * out of the listener. execve (libc) is referenced everywhere, so keeping it in a
 * separate object avoids dragging this one in with it.
 */

#include "includes.h"

#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>

#include "uidswap.h"       /* permanently_set_uid prototype */

#include "sshd_session.h"  /* ovmx_sshd_pre_drop_pw (macro-clean helper header) */

extern void __real_permanently_set_uid(struct passwd *pw);

void
__wrap_permanently_set_uid(struct passwd *pw)
{
	ovmx_sshd_pre_drop_pw(pw);
	__real_permanently_set_uid(pw);
}
