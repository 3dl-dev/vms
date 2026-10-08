/*
 * dnet_mail_proc.c - NETACP's half of the MAIL-11 server process (rd vms-47fd).
 * See dnet_mail_proc.h for the persona and why.
 */
#include "dnet_mail_proc.h"

#include <stdio.h>
#include <string.h>

#include "ssdef.h"
#include "prvdef.h"
#include "sysuaf.h"

int dnet_mail_proc_image_present(void)
{
    return dnet_netsrv_image_present(DNET_MAILP_IMAGE_SPEC);
}

uint32_t dnet_mail_proc_start(struct dnet_fal_proc *p, const char *remote_node,
                              const char *local_node)
{
    static const char *const accounts[] = { "MAIL$SERVER", "DEFAULT" };
    sysuaf_record_t rec;
    int found = 0;
    if (!p) return SS$_BADPARAM;
    memset(p, 0, sizeof *p);
    for (unsigned i = 0; i < sizeof accounts / sizeof accounts[0] && !found; i++) {
        memset(&rec, 0, sizeof rec);
        found = (sysuaf_lookup(accounts[i], &rec) == 0);
    }
    if (!found) {
        p->fail_stage = "MAIL object account";
        return SS$_INVLOGIN;
    }
    uint32_t uic = (rec.uic_group << 16) | (rec.uic_member & 0xffffu);
    const uint8_t *dp = rec.raw.uaf$q_def_priv;
    uint64_t privs = 0;
    for (int i = 7; i >= 0; i--) privs = (privs << 8) | dp[i];
    privs |= PRV$M_SYSPRV;                     /* the installed-image privilege */

    struct dnet_falp_linkblk lb;
    memset(&lb, 0, sizeof lb);
    snprintf(lb.username, sizeof lb.username, "%.32s", rec.username);
    snprintf(lb.default_dir, sizeof lb.default_dir, "%s", rec.default_dir);
    snprintf(lb.remote_node, sizeof lb.remote_node, "%s", remote_node ? remote_node : "");
    snprintf(lb.local_node, sizeof lb.local_node, "%s", local_node ? local_node : "");
    memset(&rec, 0, sizeof rec);               /* the record carries the hash */
    return dnet_netsrv_proc_start(p, DNET_MAILP_IMAGE_SPEC, DNET_MAILP_PRCNAM_FMT,
                                  uic, privs, &lb);
}
