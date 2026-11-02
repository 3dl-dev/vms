/*
 * crtl_exec.c - the DEC C RTL's subprocess functions over the VMS executive
 *               (rd vms-e70f): exec*() in a vfork context, wait()/waitpid(),
 *               getpid().
 *
 * WHAT DEC C DOES (VSI C RTL Reference, "Subprocess functions"; HP C RTL
 * Reference chapter 5): vfork() does not fork -- the code between vfork() and
 * exec*() runs in the parent -- and exec*() creates a SUBPROCESS that runs the
 * named image, then returns control to the vfork() point with the subprocess's
 * PID. The child inherits the parent's open files at the point exec*() is
 * called. The C RTL passes the child its argument and environment vectors
 * through a mailbox (VAXC$EXECMBX, reserved to the C RTL), which the child's C
 * RTL reads at image start. wait()/waitpid() return the child's completion; with
 * _VMS_WAIT (DECC$__VMS_WAITPID) the status is the child's OpenVMS completion
 * code itself. getpid() is the VMS process ID.
 *
 * HOW OVMX DOES IT -- VMS system services only, no substrate fork/execve/wait4:
 *
 *   create   SYS$CREPRC of the image as a SUBPROCESS (the creator's identity,
 *            a VMS PID the executive assigns), created hibernating (PRC$M_HIBER)
 *            so its C RTL cannot look for its context before it exists. $CREPRC
 *            gives a subprocess the creator's open streams as they are at this
 *            point -- the exec-time inheritance DEC C documents.
 *   context  a temporary mailbox ($CREMBX) whose logical name, in LNM$JOB
 *            (LNM$TEMPORARY_MAILBOX; the subprocess shares its creator's job),
 *            is VAXC$EXECMBX_<child PID in hex>. OVMX names the mailbox by the
 *            child's PID rather than by the one name VAXC$EXECMBX because
 *            OVMX's $CREPRC does not copy the creator's LNM$PROCESS into the
 *            subprocess (LIB$SPAWN's copy is how the VMS child finds the single
 *            name); the child knows its own PID. Records: "<argc> <envc>", then
 *            one record per argument ('A' + text) and per environment string
 *            ('E' + text). Then $WAKE.
 *   child    at image start (decc$main, crtl_rms_fd.c) the C RTL $ASSIGNs its
 *            VAXC$EXECMBX_<own PID>; no such name -> the image was not exec'd.
 *   wait     the executive's subprocess-completion notification (the one
 *            LIB$SPAWN/NOWAIT arms: an event flag set when the subprocess
 *            records its exit or is deleted) and the termination record the
 *            executive keeps for the creator ($STATUS, SS$_ABORT when the image
 *            recorded none). No substrate wait.
 *
 * WAIT STATUS (POSIX encoding, what <wait.h> WEXITSTATUS decodes without
 * _VMS_WAIT). The value DEC C documents is the low eight bits of what the child
 * passed to exit(). The child's completion code is that value (DECC$EXIT passes
 * it through, 0 -> SS$_NORMAL) or, under _POSIX_EXIT, the C-facility code
 * %X0035A001 + (n << 3) (%X1035A00A for 1) -- both measured on OpenVMS Alpha
 * V8.4, tests/lab/captures/decc-exit-alpha-20261021. So: a C-facility code gives
 * back n exactly; any other success gives 0; any other failure gives its low
 * eight bits (1 when those are 0, so a failure is never reported as success).
 * The full completion code is what DECC$__VMS_WAITPID returns.
 *
 * NOT YET (rd items filed): the substrate task of a $CREPRC subprocess is left
 * for the creator to reap at image exit (the executive deletes the VMS process
 * at once; only the substrate's record lingers); file positions and signal
 * actions are not part of the context (the child has the descriptors
 * themselves); getppid() is unchanged ($GETJPI JPI$_OWNER is not served yet).
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ssdef.h"
#include "descrip.h"
#include "iodef.h"
#include "prcdef.h"
#include "lnmdef.h"
#include "starlet.h"

#include "crtl_exec.h"

/* LIB$GET_EF / the executive's completion notification and termination record
 * (LIBVMS$SHR / LIBVMSSYS$SHR -- the same two LIB$SPAWN/NOWAIT uses). */
extern uint32_t lib$get_ef(uint32_t *efn);
/* crtl_devfd.c; weak: an image (a test) built without the device layer has no
 * C RTL pipe ends to hand over */
extern int ovmx_crtl_devfd_give(long long fd, char *dev, size_t devsz) __attribute__((__weak__));
extern uint32_t vms_kif_spawn_notify(uint32_t child_vms_pid, uint32_t efn,
                                     uint64_t astadr, uint64_t astprm, int *completed);
extern uint32_t vms_kif_getexit_pid(uint32_t vms_pid, uint32_t *condition,
                                    int *has_exited);

#define EXEC_MAXCHILD 64
#define EXEC_LOGNAM   "VAXC$EXECMBX_"

/* The subprocesses this image created and has not yet waited for. The
 * context mailbox's channel is held until then: a temporary mailbox lives as
 * long as a channel to it, and the child may not have read it yet. */
static struct { uint32_t pid; uint16_t chan; } kids[EXEC_MAXCHILD];
static uint32_t exec_efn;

static struct dsc$descriptor_s sdsc(const char *s, size_t n)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length = (uint16_t)n;
    d.dsc$b_dtype = DSC$K_DTYPE_T;
    d.dsc$b_class = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

static void exec_lognam(char *buf, size_t sz, uint32_t pid)
{
    snprintf(buf, sz, EXEC_LOGNAM "%08X", (unsigned)pid);
}

static int errno_of(uint32_t st)
{
    switch (st) {
    case SS$_NOSUCHDEV: return ENODEV;
    case SS$_NOPRIV:    return EPERM;
    case SS$_EXQUOTA:
    case SS$_INSFMEM:   return EAGAIN;
    default:            return EIO;
    }
}

static uint32_t exec_put(uint16_t chan, char tag, const char *s, char *rec, size_t cap)
{
    size_t n = strlen(s);
    uint16_t iosb[4] = { 0, 0, 0, 0 };
    if (n + 1 > cap)
        return SS$_BADPARAM;
    rec[0] = tag;
    memcpy(rec + 1, s, n);
    uint32_t st = sys$qiow(0, chan, IO$_WRITEVBLK | IO$M_NOW, iosb, 0, 0,
                           rec, n + 1, 0, 0, 0, 0);
    return (st & 1) ? iosb[0] : st;
}

/* Create the subprocess. Returns its VMS PID, or 0 with errno set (nothing
 * remains: a subprocess whose context could not be given it is deleted). */
uint32_t ovmx_crtl_exec_create(const char *spec, char *const *args, size_t nargs,
                               char *const *envp)
{
    int slot = -1;
    for (int i = 0; i < EXEC_MAXCHILD; i++)
        if (!kids[i].pid) { slot = i; break; }
    if (slot < 0) {
        errno = EAGAIN;                     /* the subprocess quota of this image */
        return 0;
    }
    size_t nenv = 0, maxrec = 32, total = 32;
    if (envp)
        while (envp[nenv])
            nenv++;
    for (size_t i = 0; i < nargs; i++) {
        size_t l = strlen(args[i] ? args[i] : "") + 1;
        if (l > maxrec) maxrec = l;
        total += l + 16;
    }
    for (size_t i = 0; i < nenv; i++) {
        size_t l = strlen(envp[i]) + 1;
        if (l > maxrec) maxrec = l;
        total += l + 16;
    }
    if (maxrec > 65535 || total > 0x7FFFFFFF) {
        errno = E2BIG;
        return 0;
    }
    char *rec = malloc(maxrec + 1);
    if (!rec) {
        errno = ENOMEM;
        return 0;
    }

    uint32_t pid = 0;
    struct dsc$descriptor_s img = sdsc(spec, strlen(spec));
    /* vms-a66e: a standard descriptor that is a pipe end (a mailbox,
     * crtl_devfd.c) is the subprocess's SYS$INPUT / SYS$OUTPUT / SYS$ERROR,
     * named to $CREPRC as DEC C names it; the write end is the subprocess's to
     * end. Any other stream the subprocess inherits as it is. */
    char sdev[3][64];
    struct dsc$descriptor_s sd[3];
    const struct dsc$descriptor_s *sp[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; i++) {
        if (ovmx_crtl_devfd_give && ovmx_crtl_devfd_give(i, sdev[i], sizeof sdev[i]) &&
            strncmp(sdev[i] + (sdev[i][0] == '_'), "MBA", 3) == 0) {
            sd[i] = sdsc(sdev[i], strlen(sdev[i]));
            sp[i] = &sd[i];
        }
    }
    uint32_t st = sys$creprc(&pid, &img, sp[0], sp[1], sp[2], 0, 0, 0, 0, 0, 0, PRC$M_HIBER);
    if (!(st & 1) || !pid) {
        free(rec);
        errno = errno_of(st);
        return 0;
    }

    char ln[32];
    exec_lognam(ln, sizeof ln, pid);
    struct dsc$descriptor_s lnd = sdsc(ln, strlen(ln));
    uint16_t chan = 0;
    st = sys$crembx(0, &chan, (uint32_t)maxrec, (uint32_t)total, 0, 0, &lnd, 0, 0);
    if (st & 1) {
        char hdr[32];
        snprintf(hdr, sizeof hdr, "%u %u", (unsigned)nargs, (unsigned)nenv);
        uint16_t iosb[4] = { 0, 0, 0, 0 };
        st = sys$qiow(0, chan, IO$_WRITEVBLK | IO$M_NOW, iosb, 0, 0,
                      hdr, strlen(hdr), 0, 0, 0, 0);
        if (st & 1)
            st = iosb[0];
        for (size_t i = 0; (st & 1) && i < nargs; i++)
            st = exec_put(chan, 'A', args[i] ? args[i] : "", rec, maxrec + 1);
        for (size_t i = 0; (st & 1) && i < nenv; i++)
            st = exec_put(chan, 'E', envp[i], rec, maxrec + 1);
    }
    free(rec);
    if (!(st & 1)) {
        if (chan)
            (void)sys$dassgn(chan);
        (void)sys$delprc(&pid, 0);
        errno = errno_of(st);
        return 0;
    }
    kids[slot].pid = pid;
    kids[slot].chan = chan;
    (void)sys$wake(&pid, 0);
    return pid;
}

/* The child's completion, if it has one yet: 1 and *cond when it has ended
 * (recorded its exit, or was deleted -- SS$_ABORT when it recorded nothing). */
static int exec_done(uint32_t pid, uint32_t *cond)
{
    uint32_t c = 0;
    int rec = 0;
    uint32_t st = vms_kif_getexit_pid(pid, &c, &rec);
    if ((st & 1) && rec) {
        *cond = c;
        return 1;
    }
    uint32_t got = 0;
    uint16_t len = 0;
    struct item_list_3 il[2];
    memset(il, 0, sizeof il);
    il[0].buflen = 4;
    il[0].item_code = JPI$_PID;
    il[0].bufaddr = &got;
    il[0].retlen = &len;
    uint32_t jst = sys$getjpiw(0, &pid, 0, il, 0, 0, 0);
    if (jst == SS$_NONEXPR) {
        *cond = (st & 1) ? c : SS$_ABORT;
        return 1;
    }
    return 0;
}

static void exec_forget(int i)
{
    if (kids[i].chan)
        (void)sys$dassgn(kids[i].chan);
    kids[i].pid = 0;
    kids[i].chan = 0;
}

/* Wait for subprocess `pid` (-1 or 0: any of this image's). Returns the PID
 * and *cond, 0 for WNOHANG with none finished, or -1 with errno (ECHILD). */
int ovmx_crtl_wait_cond(int pid, uint32_t *cond, int nohang)
{
    if (!exec_efn && !(lib$get_ef(&exec_efn) & 1)) {
        errno = EAGAIN;
        return -1;
    }
    for (;;) {
        int any = 0;
        (void)sys$clref(exec_efn);
        for (int i = 0; i < EXEC_MAXCHILD; i++) {
            if (!kids[i].pid || (pid > 0 && kids[i].pid != (uint32_t)pid))
                continue;
            any = 1;
            uint32_t c;
            if (exec_done(kids[i].pid, &c)) {
                int r = (int)kids[i].pid;
                exec_forget(i);
                *cond = c;
                return r;
            }
            /* Ask the executive to set the flag when this one ends; one that
             * ended between the look above and the arm completes at once. */
            int completed = 0;
            uint32_t a = vms_kif_spawn_notify(kids[i].pid, exec_efn, 0, 0, &completed);
            if (completed || a == SS$_NONEXPR)
                (void)sys$setef(exec_efn);
        }
        if (!any) {
            errno = ECHILD;
            return -1;
        }
        if (nohang)
            return 0;
        (void)sys$waitfr(exec_efn);
    }
}

/* The child's completion code as DEC C's POSIX wait status (see the header). */
int ovmx_crtl_wait_status(uint32_t cond)
{
    unsigned int code;
    uint32_t c = cond & 0x0FFFFFFFu;
    if ((c & 0x0FFF0000u) == 0x00350000u && (c & 0xFFF8u) >= 0xA000u &&
        (c & 0xFFF8u) <= 0xA7F8u)
        code = ((c & 0xFFF8u) - 0xA000u) >> 3;      /* C$_EXIT: __posix_exit(n) */
    else if (cond & 1u)
        code = 0;
    else
        code = (cond & 0xFFu) ? (cond & 0xFFu) : 1u;
    return (int)(code << 8);
}

int ovmx_crtl_waitpid(int pid, int *status, int options)
{
    uint32_t cond = 0;
    int r = ovmx_crtl_wait_cond(pid, &cond, (options & 1) != 0 /* WNOHANG */);
    if (r > 0 && status)
        *status = ovmx_crtl_wait_status(cond);
    return r;
}

/* DECC$__VMS_WAITPID: the _VMS_WAIT form -- *status is the completion code. */
int ovmx_crtl_vms_waitpid(int pid, int *status, int options)
{
    uint32_t cond = 0;
    int r = ovmx_crtl_wait_cond(pid, &cond, (options & 1) != 0);
    if (r > 0 && status)
        *status = (int)cond;
    return r;
}

int ovmx_crtl_wait(int *status)
{
    return ovmx_crtl_waitpid(-1, status, 0);
}

/* getpid(): the VMS process ID, from the executive. A task with no VMS
 * process has no VMS PID: 0, never a substrate number in its place. */
int ovmx_crtl_getpid(void)
{
    uint32_t pid = 0;
    uint16_t len = 0;
    struct item_list_3 il[2];
    memset(il, 0, sizeof il);
    il[0].buflen = 4;
    il[0].item_code = JPI$_PID;
    il[0].bufaddr = &pid;
    il[0].retlen = &len;
    if (!(sys$getjpiw(0, 0, 0, il, 0, 0, 0) & 1))
        return 0;
    return (int)pid;
}

/* exec*() outside a vfork context (DEC C): the subprocess runs the image, the
 * caller waits for it and then exits with its completion. */
void ovmx_crtl_exec_replace(uint32_t pid)
{
    uint32_t cond = SS$_ABORT;
    if (ovmx_crtl_wait_cond((int)pid, &cond, 0) <= 0)
        cond = SS$_ABORT;
    (void)sys$exit(cond);
}

/* ---------------------------------------------------------- child side ---- */

/* At image start: the context the creator's exec*() left for this process, if
 * it was created by one. Returns 1 and the vectors (malloc'd, NULL-terminated;
 * argv[0..argc) are the ARGUMENTS, without an argv[0]), or 0 when this image
 * was not exec'd. A context that cannot be read whole is not used at all. */
int ovmx_crtl_exec_context(int *argc, char ***argv, char ***envp)
{
    uint32_t me = (uint32_t)ovmx_crtl_getpid();
    if (!me)
        return 0;
    char ln[32];
    exec_lognam(ln, sizeof ln, me);
    struct dsc$descriptor_s lnd = sdsc(ln, strlen(ln));
    uint16_t chan = 0;
    if (!(sys$assign(&lnd, &chan, 0, 0) & 1))
        return 0;                               /* not created by exec*() */

    char *buf = malloc(65536 + 1);
    char **av = 0, **ev = 0;
    unsigned na = 0, ne = 0;
    int ok = 0;
    uint16_t iosb[4];
    if (!buf)
        goto out;
    memset(iosb, 0, sizeof iosb);
    if (!(sys$qiow(0, chan, IO$_READVBLK, iosb, 0, 0, buf, 65536, 0, 0, 0, 0) & 1) ||
        !(iosb[0] & 1))
        goto out;
    buf[iosb[1]] = 0;
    if (sscanf(buf, "%u %u", &na, &ne) != 2 || na > 65536 || ne > 65536)
        goto out;
    av = calloc(na + 1, sizeof *av);
    ev = calloc(ne + 1, sizeof *ev);
    if (!av || !ev)
        goto out;
    for (unsigned i = 0; i < na + ne; i++) {
        memset(iosb, 0, sizeof iosb);
        if (!(sys$qiow(0, chan, IO$_READVBLK, iosb, 0, 0, buf, 65536, 0, 0, 0, 0) & 1) ||
            !(iosb[0] & 1) || iosb[1] < 1)
            goto out;
        char want = i < na ? 'A' : 'E';
        if (buf[0] != want)
            goto out;
        char *s = malloc(iosb[1]);
        if (!s)
            goto out;
        memcpy(s, buf + 1, iosb[1] - 1u);
        s[iosb[1] - 1u] = 0;
        if (i < na)
            av[i] = s;
        else
            ev[i - na] = s;
    }
    ok = 1;
out:
    (void)sys$dassgn(chan);
    free(buf);
    if (!ok) {
        for (unsigned i = 0; av && i < na; i++) free(av[i]);
        for (unsigned i = 0; ev && i < ne; i++) free(ev[i]);
        free(av);
        free(ev);
        return 0;
    }
    *argc = (int)na;
    *argv = av;
    *envp = ev;
    return 1;
}
