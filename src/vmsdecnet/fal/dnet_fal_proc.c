/*
 * dnet_fal_proc.c - the FAL network server process (rd vms-d85). See
 * dnet_fal_proc.h for why the access runs in its own process with the
 * authenticated user's UIC and privileges, and for the mailbox link framing.
 *
 * Rule 9 / INV-6: every step goes through the executive ($CREMBX, $CREPRC,
 * $GETJPI, mailbox $QIOs via vms_kif_*). With no executive each fails with a
 * VMS status and the caller refuses the connect -- nothing falls back to
 * serving the file from NETACP.
 */
#include "dnet_fal_proc.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "vmsfs/filespec.h"   /* vmsfs_to_linux_path()                         */
#include "ovmx_layout.h"      /* ovmx_boot_stage_exec_path                     */
#include "ssdef.h"
#include "starlet.h"          /* sys$creprc, sys$setddir                       */
#include "descrip.h"
#include "prcdef.h"
#include "vms_kif.h"
#include "vms/logical.h"     /* lnm_define_login_logicals (SYS$LOGIN)        */

#define FAL_IMAGE_SPEC "SYS$SYSTEM:FAL.EXE"

/* $SETDDIR (src/libvms/syssvc/sys_misc.c). Declared here: starlet.h carries no
 * prototype (a corpus program declares its own, conflicting one), and the
 * Alpha toolchain rejects the implicit declaration. */
extern uint32_t sys$setddir(const struct dsc$descriptor_s *new_dir,
                            unsigned short *old_len,
                            struct dsc$descriptor_s *old_dir);

static int put_rec(uint32_t ch, uint8_t type, const void *p, size_t n)
{
    uint8_t rec[DNET_FALP_MAXMSG];
    if (n + 1 > sizeof rec) return -1;
    rec[0] = type;
    if (n) memcpy(rec + 1, p, n);
    return (vms_kif_mbx_write(ch, rec, (uint32_t)(n + 1)) & 1) ? 0 : -1;
}

/* ---- NETACP side ---------------------------------------------------------- */

/* No runnable image: the caller refuses the connect, honestly (SS$_NOSUCHFILE),
 * rather than $CREPRC a server whose image activation then fails after the
 * creation already reported success -- a server that never answers. Resolved
 * the way JOB_CONTROL and the CTERM host resolve LOGINOUT.EXE (translator,
 * then the boot staging). */
static int srv_image_path(const char *spec, char *out, size_t outsz);

int dnet_fal_proc_image_present(void)
{
    return dnet_netsrv_image_present(FAL_IMAGE_SPEC);
}

/* SYS$SYSTEM:<image> -> an execve-able path (see the declaration above). */
static int srv_image_path(const char *spec, char *out, size_t outsz)
{
    char staged[512];
    if (vmsfs_to_linux_path(spec, out, outsz) != 1)
        snprintf(out, outsz, "%s", spec);
    if (out[0] == '\0')
        return 0;
    if (ovmx_boot_stage_exec_path(out, staged, sizeof(staged)) &&
        access(staged, X_OK) == 0) {
        snprintf(out, outsz, "%s", staged);
        return 1;
    }
    return access(out, X_OK) == 0 ? 1 : 0;
}

int dnet_netsrv_image_present(const char *image_spec)
{
    char img[512];
    return image_spec && srv_image_path(image_spec, img, sizeof img);
}

uint32_t dnet_netsrv_proc_start(struct dnet_fal_proc *p, const char *image_spec,
                                const char *prcnam_fmt, uint32_t uic, uint64_t privs,
                                const struct dnet_falp_linkblk *lbin)
{
    if (!p || !image_spec || !prcnam_fmt || !lbin || uic == 0)
        return SS$_BADPARAM;
    memset(p, 0, sizeof *p);

    char devto[32] = {0}, devfrom[32] = {0};
    uint32_t st = vms_kif_mbx_create(0, DNET_FALP_MAXMSG, DNET_FALP_MAXMSG * 8,
                                     &p->ch_to, &p->unit_to, devto, sizeof devto);
    if (!(st & 1)) { p->fail_stage = "$CREMBX"; return st; }
    st = vms_kif_mbx_create(0, DNET_FALP_MAXMSG, DNET_FALP_MAXMSG * 8,
                            &p->ch_from, &p->unit_from, devfrom, sizeof devfrom);
    if (!(st & 1)) {
        vms_kif_mbx_delmbx(p->ch_to);
        memset(p, 0, sizeof *p);
        p->fail_stage = "$CREMBX";
        return st;
    }

    /* The link block goes in FIRST, so it is waiting when the server starts. */
    struct dnet_falp_linkblk lb = *lbin;
    lb.username[sizeof lb.username - 1] = '\0';
    lb.default_dir[sizeof lb.default_dir - 1] = '\0';
    lb.remote_node[sizeof lb.remote_node - 1] = '\0';
    lb.local_node[sizeof lb.local_node - 1] = '\0';
    if (put_rec(p->ch_to, DNET_FALP_REC_LINKBLK, &lb, sizeof lb) != 0) {
        dnet_fal_proc_close(p);
        p->fail_stage = "mailbox $QIO";
        return SS$_EXQUOTA;
    }

    char img[512], prcnam[16];
    if (!srv_image_path(image_spec, img, sizeof img)) {
        dnet_fal_proc_close(p);
        p->fail_stage = "image lookup";
        return SS$_NOSUCHFILE;
    }
    snprintf(prcnam, sizeof prcnam, prcnam_fmt,
             (unsigned)p->unit_to, (unsigned)p->unit_from);

    struct dsc$descriptor_s img_d = dsc$init(img);
    struct dsc$descriptor_s nam_d = dsc$init(prcnam);
    uint64_t pv = privs;
    uint32_t pid = 0;
    /* THE PERSONA is the caller's choice and only the caller's: FAL passes the
     * authenticated user's UIC + DEFAULT privileges, MAIL the MAIL object's
     * account. Never NETACP's own identity. */
    st = sys$creprc(&pid, &img_d, NULL, NULL, NULL, &pv, NULL, &nam_d,
                    0, uic, 0, PRC$M_DETACH);
    if (!(st & 1)) {
        dnet_fal_proc_close(p);
        p->fail_stage = "$CREPRC";
        return st;
    }
    p->pid = pid;
    p->uic = uic;
    p->active = 1;
    return SS$_NORMAL;
}

uint32_t dnet_fal_proc_start(struct dnet_fal_proc *p, uint32_t uic,
                             uint64_t def_privs, const char *username,
                             const char *default_dir)
{
    if (!p || !username || uic == 0)
        return SS$_BADPARAM;
    struct dnet_falp_linkblk lb;
    memset(&lb, 0, sizeof lb);
    snprintf(lb.username, sizeof lb.username, "%s", username);
    snprintf(lb.default_dir, sizeof lb.default_dir, "%s", default_dir ? default_dir : "");
    /* THE PERSONA: the server runs with the authenticated user's UIC and
     * DEFAULT privileges -- the identity the executive ACP checks every file
     * open against. Never NETACP's own. */
    return dnet_netsrv_proc_start(p, FAL_IMAGE_SPEC, DNET_FALP_PRCNAM_FMT, uic,
                                  def_privs, &lb);
}

int dnet_fal_proc_put(struct dnet_fal_proc *p, const uint8_t *seg, size_t len)
{
    if (!p || !p->active) return -1;
    return put_rec(p->ch_to, DNET_FALP_REC_DATA, seg, len);
}

int dnet_fal_proc_poll(struct dnet_fal_proc *p, uint8_t *buf, size_t cap,
                       size_t *len, uint32_t *status)
{
    if (!p || !p->active) return -1;
    uint8_t rec[DNET_FALP_MAXMSG];
    uint32_t n = 0;
    uint32_t st = vms_kif_mbx_read(p->ch_from, rec, sizeof rec, &n, 1);
    if (st == SS$_ENDOFFILE) return 0;
    if (!(st & 1) || n == 0 || n > sizeof rec) return -1;
    if (rec[0] == DNET_FALP_REC_DATA) {
        if (n - 1 > cap) return -1;
        memcpy(buf, rec + 1, n - 1);
        *len = n - 1;
        return 1;
    }
    if (rec[0] == DNET_FALP_REC_EXIT && n == 5) {
        if (status)
            *status = (uint32_t)rec[1] | ((uint32_t)rec[2] << 8) |
                      ((uint32_t)rec[3] << 16) | ((uint32_t)rec[4] << 24);
        return 2;
    }
    return -1;   /* an unknown record from the server ends the session */
}

int dnet_fal_proc_alive(const struct dnet_fal_proc *p)
{
    struct vms_procinfo info;
    if (!p || !p->active || p->pid == 0) return 0;
    memset(&info, 0, sizeof info);
    return (vms_kif_getjpi_pid(p->pid, &info) & 1) ? 1 : 0;
}

void dnet_fal_proc_close(struct dnet_fal_proc *p)
{
    if (!p) return;
    if (p->ch_to) {
        (void)put_rec(p->ch_to, DNET_FALP_REC_END, NULL, 0);
        vms_kif_mbx_delmbx(p->ch_to);
    }
    if (p->ch_from) vms_kif_mbx_delmbx(p->ch_from);
    memset(p, 0, sizeof *p);
}

/* ---- server-image side (FAL.EXE, MAIL_SERVER.EXE) ------------------------- */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Blocking-with-idle-bound read of the next record from NETACP. */
static int link_read(struct dnet_netsrv_link *l, uint8_t *rec, size_t cap, uint32_t *n)
{
    uint64_t deadline = now_ms() + (uint64_t)DNET_FALP_IDLE_SEC * 1000u;
    for (;;) {
        uint32_t st = vms_kif_mbx_read(l->ch_in, rec, (uint32_t)cap, n, 1);
        if (st & 1) return (*n > 0 && *n <= cap) ? 0 : -1;
        if (st != SS$_ENDOFFILE) return -1;
        if (now_ms() > deadline) return -1;      /* idle: end the session */
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
}

uint32_t dnet_netsrv_attach(const char *prcnam_scanfmt, struct dnet_netsrv_link *l,
                            struct dnet_falp_linkblk *lb)
{
    /* 1. Which link is mine? My own process name, read from the executive. */
    struct vms_procinfo me;
    memset(&me, 0, sizeof me);
    memset(l, 0, sizeof *l);
    if (!(vms_kif_getjpi_self(&me) & 1)) return SS$_NOSUCHDEV;
    unsigned uto = 0, ufrom = 0;
    char nm[sizeof me.prcnam + 1];
    memcpy(nm, me.prcnam, sizeof me.prcnam);
    nm[sizeof me.prcnam] = '\0';
    if (sscanf(nm, prcnam_scanfmt, &uto, &ufrom) != 2) return SS$_BADPARAM;

    char dev[32];
    snprintf(dev, sizeof dev, "MBA%u:", uto);
    if (!(vms_kif_mbx_assign(dev, &l->ch_in) & 1)) return SS$_NOSUCHDEV;
    snprintf(dev, sizeof dev, "MBA%u:", ufrom);
    if (!(vms_kif_mbx_assign(dev, &l->ch_out) & 1)) return SS$_NOSUCHDEV;

    /* 2. The link block: who this access is for, and its SYS$LOGIN. */
    uint8_t rec[DNET_FALP_MAXMSG];
    uint32_t n = 0;
    if (link_read(l, rec, sizeof rec, &n) != 0 || rec[0] != DNET_FALP_REC_LINKBLK ||
        n != 1 + sizeof(struct dnet_falp_linkblk))
        return SS$_BADPARAM;
    memcpy(lb, rec + 1, sizeof *lb);
    lb->username[sizeof lb->username - 1] = '\0';
    lb->default_dir[sizeof lb->default_dir - 1] = '\0';
    lb->remote_node[sizeof lb->remote_node - 1] = '\0';
    lb->local_node[sizeof lb->local_node - 1] = '\0';
    return SS$_NORMAL;
}

int dnet_netsrv_send(struct dnet_netsrv_link *l, const uint8_t *seg, size_t len)
{
    return put_rec(l->ch_out, DNET_FALP_REC_DATA, seg, len);
}

int dnet_netsrv_recv(struct dnet_netsrv_link *l, uint8_t *buf, size_t cap, size_t *len)
{
    uint8_t rec[DNET_FALP_MAXMSG];
    uint32_t n = 0;
    if (l->ended || link_read(l, rec, sizeof rec, &n) != 0) return -1;
    if (rec[0] != DNET_FALP_REC_DATA) { l->ended = 1; return -1; }
    if (n - 1 > cap) return -1;
    if (n > 1) memcpy(buf, rec + 1, n - 1);
    *len = n - 1;
    return 0;
}

void dnet_netsrv_exit(struct dnet_netsrv_link *l, uint32_t status)
{
    uint8_t sb[4] = { (uint8_t)status, (uint8_t)(status >> 8),
                      (uint8_t)(status >> 16), (uint8_t)(status >> 24) };
    (void)put_rec(l->ch_out, DNET_FALP_REC_EXIT, sb, sizeof sb);
}

static int fal_tsend(void *ctx, const uint8_t *seg, size_t len)
{
    return dnet_netsrv_send(ctx, seg, len);
}

static int fal_trecv(void *ctx, uint8_t *buf, size_t cap, size_t *len)
{
    return dnet_netsrv_recv(ctx, buf, cap, len);
}

uint32_t dnet_fal_proc_serve(void)
{
    static struct dnet_dap_transport t;
    static struct dnet_netsrv_link l;
    struct dnet_falp_linkblk lb;
    uint32_t ast = dnet_netsrv_attach("FAL_%u_%u", &l, &lb);
    if (!(ast & 1)) return ast;

    /* A relative filespec from the peer resolves in the USER's login
     * directory, as a VMS network job's does: $SETDDIR sets the executive's
     * default directory, which RMS completes every relative spec from
     * (rd vms-872). */
    if (lb.default_dir[0]) {
        struct dsc$descriptor_s d = dsc$init(lb.default_dir);
        (void)sys$setddir(&d, NULL, NULL);
        /* The network job's SYS$LOGIN / SYS$LOGIN_DEVICE, job-wide, from the
         * SYSUAF record -- the same LNM$JOB logicals a LOGINOUT session gets
         * (lnm_define_login_logicals, vms-e48), so a peer's
         * "SYS$LOGIN:FILE.TXT" names this user's home, not the system-wide
         * [USERS] default. */
        lnm_manager_t *mgr = lnm_get_manager();
        if (mgr)
            (void)lnm_define_login_logicals(mgr, LNM_JOB_TABLE, lb.default_dir);
    }

    /* 3. The DAP session, every file open checked against THIS process's
     * identity (the user's UIC + default privileges). */
    t.send = fal_tsend;
    t.recv = fal_trecv;
    t.ctx  = &l;
    uint32_t status = dnet_fal_server_run(&t);
    dnet_netsrv_exit(&l, status);
    return status;
}
