/*
 * priv_display.h - VMS Privilege Bitmask-to-Name Rendering
 *
 * The reverse of parse_privilege_string() in privs.h: turns a privilege
 * bitmask into the names and descriptions VMS prints. Provided as static
 * inlines in a shared header for the same reason privs.h is -- so DCL's
 * SHOW PROCESS output and any test agree by construction rather than by
 * two hand-maintained tables. Two hand-maintained tables is exactly how
 * DCL came to display a privilege set that shared no bit values with
 * prvdef.h: a process holding TMPMBX (prvdef bit 15) was reported as
 * holding DETACH, and everything above bit 24 -- SYSPRV, BYPASS, READALL --
 * could not be displayed at all.
 *
 * BITS: prvdef.h, the single source of truth, also enforced by the
 * executive (see the _Static_asserts in src/vmsprocess/access_modes.c).
 *
 * NAMES, DESCRIPTIONS and the layout: pinned to the reference lab
 * (~/vax/cluster, OpenVMS VAX V7.3, node VAX1). The text below is SHOW
 * PROCESS/PRIVILEGES output captured there verbatim:
 *      Process privileges:
 *       CMKRNL               may change mode to kernel
 *       SETPRV               may set any privilege bit
 * i.e. one leading space, name left-justified in 20 columns, a space, then
 * the description. Descriptions are NOT paraphrased.
 *
 * DETACH and SETPRI carry NO description: they were absent from the
 * observed output (SYSTEM on that node is not authorized for them), so
 * nothing is pinned and OVMX does not invent one. AUDIT and IMPORT appear
 * on the lab but have no prvdef.h bit, so they cannot be rendered here;
 * both gaps are reported for follow-up rather than guessed at.
 */

#ifndef __VMS_PRIV_DISPLAY_H
#define __VMS_PRIV_DISPLAY_H

#include <stdint.h>
#include <stdio.h>
#include "prvdef.h"

struct vms_priv_display_ent {
    const char *name;
    uint64_t    bit;
    const char *desc;    /* NULL = description not pinned to the oracle */
};

static const struct vms_priv_display_ent vms_priv_display[] = {
    { "ACNT",        PRV$M_ACNT,        "may suppress accounting messages" },
    { "ALLSPOOL",    PRV$M_ALLSPOOL,    "may allocate spooled device" },
    { "ALTPRI",      PRV$M_ALTPRI,      "may set any priority value" },
    { "BUGCHK",      PRV$M_BUGCHK,      "may make bug check log entries" },
    { "BYPASS",      PRV$M_BYPASS,      "may bypass all object access controls" },
    { "CMEXEC",      PRV$M_CMEXEC,      "may change mode to exec" },
    { "CMKRNL",      PRV$M_CMKRNL,      "may change mode to kernel" },
    { "DETACH",      PRV$M_DETACH,      NULL },
    { "DIAGNOSE",    PRV$M_DIAGNOSE,    "may diagnose devices" },
    { "DOWNGRADE",   PRV$M_DOWNGRADE,   "may downgrade object secrecy" },
    { "EXQUOTA",     PRV$M_EXQUOTA,     "may exceed disk quota" },
    { "GROUP",       PRV$M_GROUP,       "may affect other processes in same group" },
    { "GRPNAM",      PRV$M_GRPNAM,      "may insert in group logical name table" },
    { "GRPPRV",      PRV$M_GRPPRV,      "may access group objects via system protection" },
    { "IMPERSONATE", PRV$M_IMPERSONATE, "may impersonate another user" },
    { "LOG_IO",      PRV$M_LOG_IO,      "may do logical i/o" },
    { "MOUNT",       PRV$M_MOUNT,       "may execute mount acp function" },
    { "NETMBX",      PRV$M_NETMBX,      "may create network device" },
    { "OPER",        PRV$M_OPER,        "may perform operator functions" },
    { "PFNMAP",      PRV$M_PFNMAP,      "may map to specific physical pages" },
    { "PHY_IO",      PRV$M_PHY_IO,      "may do physical i/o" },
    { "PRMCEB",      PRV$M_PRMCEB,      "may create permanent common event clusters" },
    { "PRMGBL",      PRV$M_PRMGBL,      "may create permanent global sections" },
    { "PRMMBX",      PRV$M_PRMMBX,      "may create permanent mailbox" },
    { "PSWAPM",      PRV$M_PSWAPM,      "may change process swap mode" },
    { "READALL",     PRV$M_READALL,     "may read anything as the owner" },
    { "SECURITY",    PRV$M_SECURITY,    "may perform security administration functions" },
    { "SETPRI",      PRV$M_SETPRI,      NULL },
    { "SETPRV",      PRV$M_SETPRV,      "may set any privilege bit" },
    { "SHARE",       PRV$M_SHARE,       "may assign channels to non-shared devices" },
    { "SHMEM",       PRV$M_SHMEM,       "may create/delete objects in shared memory" },
    { "SYSGBL",      PRV$M_SYSGBL,      "may create system wide global sections" },
    { "SYSLCK",      PRV$M_SYSLCK,      "may lock system wide resources" },
    { "SYSNAM",      PRV$M_SYSNAM,      "may insert in system logical name table" },
    { "SYSPRV",      PRV$M_SYSPRV,      "may access objects via system protection" },
    { "TMPMBX",      PRV$M_TMPMBX,      "may create temporary mailbox" },
    { "UPGRADE",     PRV$M_UPGRADE,     "may upgrade object integrity" },
    { "VOLPRO",      PRV$M_VOLPRO,      "may override volume protection" },
    { "WORLD",       PRV$M_WORLD,       "may affect other processes in the world" },
    { NULL, 0, NULL }
};

/*
 * Render the "Process privileges:" detail block for `mask` into `out`.
 * Returns the number of privileges rendered. A mask of 0 renders the
 * "(no privileges enabled)" line and returns 0 -- it must NEVER substitute
 * a default set, which is what DCL's old
 * "if (privmask == 0) privmask = TMPMBX|NETMBX" fallback did: reporting two
 * privileges that nothing had granted.
 */
static inline int vms_priv_render_detail(uint64_t mask, FILE *out)
{
    int found = 0;

    fprintf(out, "Process privileges:\n");
    for (int i = 0; vms_priv_display[i].name; i++) {
        if (!(mask & vms_priv_display[i].bit))
            continue;
        if (vms_priv_display[i].desc)
            fprintf(out, " %-20s %s\n", vms_priv_display[i].name,
                    vms_priv_display[i].desc);
        else
            fprintf(out, " %-20s\n", vms_priv_display[i].name);
        found++;
    }
    if (!found)
        fprintf(out, " (no privileges enabled)\n");

    return found;
}

/*
 * Render the one-line "Privileges:" summary used by plain SHOW PROCESS.
 * Returns the number of privilege names emitted.
 */
static inline int vms_priv_render_summary(uint64_t mask, FILE *out)
{
    int n = 0;

    fprintf(out, "Privileges:       ");
    for (int i = 0; vms_priv_display[i].name; i++) {
        if (mask & vms_priv_display[i].bit) {
            fprintf(out, " %s", vms_priv_display[i].name);
            n++;
        }
    }
    if (!n)
        fprintf(out, " (none)");
    fprintf(out, "\n");

    return n;
}

#endif /* __VMS_PRIV_DISPLAY_H */
