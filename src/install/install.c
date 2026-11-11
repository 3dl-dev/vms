/*
 * install.c - INSTALL utility: the executive's known file list
 * (bead vms-913.5; executive-resident since rd vms-7c64 / vms-220).
 *   INSTALL ADD     <image-filespec> [/OPEN] [/SHARED] [/HEADER_RESIDENT]
 *                                    [/PRIVILEGED=(priv,...)]
 *   INSTALL REPLACE <image-filespec> [qualifiers as ADD]
 *   INSTALL LIST    [<image-filespec>] [/FULL]
 *   INSTALL REMOVE  <image-filespec>
 *
 * On VMS the known file list is executive state, changed under CMKRNL. Here
 * too: every change is a VMS_IOCTL_KFE request (vms_kif_kfe), refused
 * SS$_NOPRIV without CMKRNL, and the list lives in the executive -- there is
 * no on-disk database (the former SYS$SYSTEM:VMS$KNOWN_IMAGES.DAT, which an
 * unprivileged startup could not write and anyone with that file could edit).
 * ADD copies the image: the executive itself copies the file INSTALL holds
 * open into its own read-only, executive-only directory and holds that copy,
 * writes denied, for the entry's life; the entry and its privileges name the
 * copy, so any image can be installed and nothing a process does afterwards
 * changes an installed one (Baron's rulings, rd vms-96e7, vms-220).
 *
 * Output follows the lab VMS captures (tests/lab/captures/install-priv-
 * 20261009/): LIST prints the entry with its Open/Hdr/Shar/Prv tags; /FULL
 * adds the entry access count and the privileges.
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "ovmx_layout.h"
#include "str_util.h"

#include "vms_kif.h"
#include "vms/privs.h"

#include "starlet.h"
#include "descrip.h"
#include "lnmdef.h"
#include "iodef.h"
#include "dvidef.h"
#include "dcdef.h"

/* SS$_ values the executive returns (ssdef.h numbers, kept local: this
 * utility includes no VMS headers beyond the kif). */
#define ST_NOPRIV     0x24u
#define ST_DUPLNAM    148u
#define ST_NOSUCHFILE 2320u

static const struct { const char *name; uint64_t bit; } priv_names[] = {
    { "CMKRNL", PRV$M_CMKRNL }, { "CMEXEC", PRV$M_CMEXEC }, { "SYSNAM", PRV$M_SYSNAM },
    { "GRPNAM", PRV$M_GRPNAM }, { "DETACH", PRV$M_DETACH }, { "TMPMBX", PRV$M_TMPMBX },
    { "WORLD", PRV$M_WORLD },   { "OPER", PRV$M_OPER },     { "NETMBX", PRV$M_NETMBX },
    { "SETPRV", PRV$M_SETPRV }, { "SYSPRV", PRV$M_SYSPRV }, { "BYPASS", PRV$M_BYPASS },
    { "GROUP", PRV$M_GROUP },   { "LOG_IO", PRV$M_LOG_IO }, { "PHY_IO", PRV$M_PHY_IO },
    { "ALTPRI", PRV$M_ALTPRI }, { "SETPRI", PRV$M_SETPRI },
};

/* ------------------------------------------------------------------ */
/* SYS$OUTPUT and SYS$ERROR (rd vms-d41)                               */
/* ------------------------------------------------------------------ */

/*
 * WHAT VMS DOES. INSTALL is an image DCL runs in the user's process; what it
 * writes goes to the process-permanent files. LIST's display is SYS$OUTPUT
 * records; a message ($PUTMSG) goes to SYS$ERROR and, when SYS$OUTPUT is a
 * different file, to SYS$OUTPUT as well -- so a failure is never lost when one
 * of them is the null device.
 *
 * WHAT OVMX DOES. At image start each name is translated (a process-permanent
 * file's ESC NUL IFI header dropped) to its device; a MAILBOX (a parent driving
 * this DCL, SPAWN, the QEMU harness) or a TERMINAL gets a channel ($ASSIGN), and
 * every record is one $QIOW IO$_WRITEVBLK on it -- a terminal's with carriage
 * control, as DCL writes its own. The substrate stream (stdout/stderr) carries
 * a record only where there is no such channel: no executive (the host), a
 * disk-file SYS$OUTPUT, or a terminal stream the substrate has redirected
 * underneath (a DCL PIPE segment) -- the same rule as the C RTL's standard
 * streams (src/vmsrms/crtl_devfd.c) and DCL's sink (src/vmsdcl/dcl_io.c).
 * Before this, INSTALL's messages went to the substrate's fd 2 only, which is
 * the null device under a DCL whose SYS$ERROR is a mailbox: INSTALL ADD failed
 * with no message at all.
 */
enum { PPF_SUBSTRATE = 0, PPF_MBX, PPF_TT };
struct ppf {
    int      kind;
    uint16_t chan;
    char     dev[64];        /* the device ("" for the substrate stream) */
};
static struct ppf g_ppf[2];  /* [0] SYS$OUTPUT, [1] SYS$ERROR */

static void mkdesc(struct dsc$descriptor_s *d, const char *s, size_t n)
{
    d->dsc$w_length = (uint16_t)n;
    d->dsc$b_dtype = DSC$K_DTYPE_T;
    d->dsc$b_class = DSC$K_CLASS_S;
    d->dsc$a_pointer = (char *)s;
}

/* `lognam`'s device: translated iteratively, as RMS does for a device, with a
 * process-permanent file's ESC NUL IFI header dropped. 0 when it is not a bare
 * device ("ddcu:"), e.g. a disk file, or cannot be translated. */
static int ppf_device(const char *lognam, char *out, size_t outsz)
{
    char cur[256];
    snprintf(cur, sizeof cur, "%s", lognam);
    for (int depth = 0; depth < 10; depth++) {
        struct dsc$descriptor_s d;
        struct item_list_3 il[2];
        uint16_t rl = 0;
        char eq[256];
        size_t cl = strlen(cur);
        if (cl && cur[cl - 1] == ':')
            cl--;
        mkdesc(&d, cur, cl);
        memset(il, 0, sizeof il);
        il[0].buflen = sizeof eq - 1;
        il[0].item_code = LNM$_STRING;
        il[0].bufaddr = eq;
        il[0].retlen = &rl;
        if (!(sys$trnlnm(NULL, NULL, &d, NULL, il) & 1))
            break;                          /* not a logical name: a device */
        if (rl >= sizeof eq)
            rl = sizeof eq - 1;
        eq[rl] = '\0';
        const char *v = eq;
        if (rl >= 4 && eq[0] == 0x1B && eq[1] == 0) {
            v = eq + 4;                     /* process-permanent file */
            rl = (uint16_t)(rl - 4);
        }
        if (!rl || rl >= sizeof cur || v[0] == '/')
            return 0;                       /* no VMS name behind it */
        memmove(cur, v, (size_t)rl);
        cur[rl] = '\0';
    }
    const char *col = strchr(cur, ':');
    if (!col || col[1] != '\0' || strlen(cur) >= outsz)
        return 0;
    snprintf(out, outsz, "%s", cur);
    return 1;
}

static uint32_t ppf_devclass(uint16_t chan)
{
    struct item_list_3 il[2];
    uint32_t cls = 0;
    uint16_t iosb[4] = { 0, 0, 0, 0 };
    memset(il, 0, sizeof il);
    il[0].buflen = sizeof cls;
    il[0].item_code = DVI$_DEVCLASS;
    il[0].bufaddr = &cls;
    if (sys$getdviw(0, chan, NULL, il, (void *)iosb, NULL, 0, 0) & 1)
        return cls;
    return 0;
}

static void ppf_bind(void)
{
    static const char *const names[2] = { "SYS$OUTPUT", "SYS$ERROR" };
    for (int which = 0; which < 2; which++) {
        struct ppf *p = &g_ppf[which];
        char dev[64];
        uint16_t chan = 0;
        struct dsc$descriptor_s d;
        memset(p, 0, sizeof *p);
        if (!ppf_device(names[which], dev, sizeof dev))
            continue;
        mkdesc(&d, dev, strlen(dev));
        if (!(sys$assign(&d, &chan, 0, NULL, 0) & 1))
            continue;
        uint32_t cls = ppf_devclass(chan);
        int kind = cls == DC$_MAILBOX ? PPF_MBX :
                   cls == DC$_TERM && isatty(which ? STDERR_FILENO : STDOUT_FILENO)
                       ? PPF_TT : PPF_SUBSTRATE;
        if (kind == PPF_SUBSTRATE) {
            (void)sys$dassgn(chan);
            continue;
        }
        p->kind = kind;
        p->chan = chan;
        snprintf(p->dev, sizeof p->dev, "%s", dev);
    }
}

/* One record to SYS$OUTPUT (which 0) or SYS$ERROR (1). */
static void ppf_put(int which, const char *rec)
{
    const struct ppf *p = &g_ppf[which];
    size_t n = strlen(rec);
    if (p->kind == PPF_MBX || p->kind == PPF_TT) {
        char msg[1024];
        uint16_t iosb[4] = { 0, 0, 0, 0 };
        uint32_t cc = 0;
        if (n > sizeof msg - 2)
            n = sizeof msg - 2;
        memcpy(msg, rec, n);
        if (p->kind == PPF_MBX)
            msg[n++] = '\n';         /* one line per message, as DCL's own records */
        else
            cc = ' ';                /* terminal: LF before, CR after */
        uint32_t st = sys$qiow(0, p->chan, IO$_WRITEVBLK, iosb, NULL, 0,
                               msg, n, 0, cc, 0, 0);
        if ((st & 1) && (iosb[0] & 1))
            return;
    }
    FILE *f = which ? stderr : stdout;   /* no channel: the substrate stream */
    fputs(rec, f);
    fputc('\n', f);
    fflush(f);
}

/* A message, as $PUTMSG writes it: SYS$ERROR, and SYS$OUTPUT too when that is
 * another file. */
static void msg_put(const char *line)
{
    /* NEGCTL-ANCHORED (install-msg-to-substrate-stderr) */
    ppf_put(1, line);
    if (strcasecmp(g_ppf[0].dev, g_ppf[1].dev) != 0)
        ppf_put(0, line);
}

static void msg_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void msg_printf(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    msg_put(line);
}

static void out_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out_printf(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    ppf_put(0, line);
}

/* The secondary line for a status the executive returned: INSTALL's own
 * reasons, else the system message ($GETMSG) as "-FAC-S-IDENT, text".
 * NOSUCHFILE is "Known File Entry not found" only where an entry was looked
 * up: on CREATE no entry is, and the status is the executive's copy of the
 * image failing (its directory absent) -- reported as itself, not as a
 * missing entry, which is what hid that cause (rd vms-d41). */
static void report_failure(const char *what, const char *spec, uint32_t st)
{
    const char *why =
        st == ST_NOPRIV     ? "-SYSTEM-F-NOCMKRNL, operation requires CMKRNL privilege" :
        st == ST_DUPLNAM    ? "-INSTALL-E-DUPLICATE, Known File Entry already exists" :
        st == ST_NOSUCHFILE && strcmp(what, "CREATE") != 0
                            ? "-INSTALL-E-NOKFEFND, Known File Entry not found" : NULL;
    msg_printf("%%INSTALL-E-FAIL, failed to %s entry for %s", what, spec);
    if (why) {
        msg_put(why);
        return;
    }
    char text[256];
    uint16_t len = 0;
    struct dsc$descriptor_s buf;
    mkdesc(&buf, text, sizeof text - 1);
    if ((sys$getmsg(st, &len, &buf, 0, NULL) & 1) && len > 0) {
        text[len] = '\0';
        text[0] = '-';                      /* a secondary message */
        msg_put(text);
    } else {
        msg_printf("-SYSTEM-F-ABORT, status %%X%08X", (unsigned)st);
    }
}

#define KNOWN_IMAGES_DB  VMS_SYSTEM_DIR "/VMS$KNOWN_IMAGES.DAT"

/*
 * resolve_filespec - turn an image filespec into {SONAME, Linux path}.
 *
 * Accepts "SYS$SHARE:name", "SYS$SYSTEM:name", or a bare "name" (defaults
 * to SYS$SHARE, where installed shareable images live). Any other device/
 * logical prefix is rejected — real INSTALL only manages images in the
 * known SYS$SHARE/SYS$SYSTEM locations.
 *
 * Returns 0 on success, -1 if the filespec cannot be resolved.
 */
static int resolve_filespec(const char *filespec, char *soname, size_t soname_len,
                             char *linux_path, size_t path_len)
{
    const char *colon = strrchr(filespec, ':');
    const char *dir;
    const char *name;

    if (colon) {
        char prefix[32] = {0};
        size_t plen = (size_t)(colon - filespec);
        if (plen == 0 || plen >= sizeof(prefix))
            return -1;
        memcpy(prefix, filespec, plen);
        name = colon + 1;

        if (strcasecmp(prefix, "SYS$SHARE") == 0)
            dir = VMS_LIBRARY_DIR;
        else if (strcasecmp(prefix, "SYS$SYSTEM") == 0)
            dir = VMS_SYSTEM_DIR;
        else
            return -1;
    } else {
        dir = VMS_LIBRARY_DIR;
        name = filespec;
    }

    if (name[0] == '\0')
        return -1;

    str_upcase_copy(soname, name, soname_len);

    int n = snprintf(linux_path, path_len, "%s/%s", dir, soname);
    if (n < 0 || (size_t)n >= path_len)
        return -1;

    /*
     * ATOMIC FLIP (vms-0cb): SYS$DISK is now a genuine Files-11 (ODS-2) volume
     * owned by the executive ACP; the vmsfs_to_linux_path -> /vms POSIX
     * passthrough is retired, so a SYS$SHARE / SYS$SYSTEM image has no /vms
     * path to stat. PID 1 stages the SYS$SHARE shareables (and SYS$SYSTEM
     * utilities) off the ODS-2 volume THROUGH THE ACP into OVMX_BOOT_STAGE_DIR
     * (src/ovmx_init/ovmx_init.c stage_boot_images(); INV-6: bytes from the
     * ACP, never a /vms read). INSTALL cannot ride the in-process ACP path --
     * it must stat() a real file to register it and to record the path IMGACT
     * maps -- so when the canonical /vms path is absent, resolve to the staged
     * copy instead. The staging directory is flat, keyed by the uppercase
     * basename (== SONAME), so the same rewrite covers SYS$SHARE and SYS$SYSTEM.
     *
     * When neither the /vms path nor a staged copy exists, keep the canonical
     * path: cmd_add's stat() then reports an honest %INSTALL-E-FILNOTFND naming
     * the SYS$SHARE/SYS$SYSTEM location, never a faked success. On a real /vms
     * fixture (the host INSTALL unit test) the canonical path exists and is
     * used unchanged -- the staged fallback only engages on the flipped runtime.
     */
    struct stat st;
    if (stat(linux_path, &st) != 0) {
        char staged[256];
        int m = snprintf(staged, sizeof(staged), "%s/%s",
                         OVMX_BOOT_STAGE_DIR, soname);
        if (m > 0 && (size_t)m < sizeof(staged) &&
            (size_t)m < path_len && stat(staged, &st) == 0) {
            memcpy(linux_path, staged, (size_t)m + 1);
        }
    }
    return 0;
}



/* ------------------------------------------------------------------ */
/* Subcommands                                                         */
/* ------------------------------------------------------------------ */

/* ADD and REPLACE: open the resolved image file and hand it to the executive. */
static int add_or_replace(int replace, int argc, char *argv[])
{
    if (argc < 1) {
        msg_printf("%%INSTALL-E-NOFILE, no image filespec specified");
        return 1;
    }
    struct vms_kfe_args a;
    memset(&a, 0, sizeof a);
    a.op = replace ? VMS_KFE_OP_REPLACE : VMS_KFE_OP_ADD;
    for (int i = 1; i < argc; i++) {
        if (strcasecmp(argv[i], "/OPEN") == 0)
            a.flags |= VMS_KFE_F_OPEN;
        else if (strcasecmp(argv[i], "/SHARED") == 0)
            a.flags |= VMS_KFE_F_SHARED;
        else if (strcasecmp(argv[i], "/HEADER_RESIDENT") == 0)
            a.flags |= VMS_KFE_F_HDRRES;
        else if (strncasecmp(argv[i], "/PRIVILEGED=", 12) == 0) {
            char list[256];
            const char *v = argv[i] + 12;
            size_t n = strlen(v);
            if (n && v[0] == '(' && v[n - 1] == ')') { v++; n -= 2; }
            if (n >= sizeof list) n = sizeof list - 1;
            memcpy(list, v, n);
            list[n] = '\0';
            a.privs = parse_privilege_string(list);
            a.flags |= VMS_KFE_F_PRIV;
        } else {
            msg_printf("%%INSTALL-E-IVQUAL, unrecognized qualifier \"%s\"", argv[i]);
            return 1;
        }
    }
    char soname[64] = {0}, path[256] = {0};
    if (resolve_filespec(argv[0], soname, sizeof soname, path, sizeof path) != 0) {
        msg_printf("%%INSTALL-E-BADFILE, cannot resolve \"%s\"", argv[0]);
        return 1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        msg_printf("%%INSTALL-E-FILNOTFND, file %s not found", path);
        return 1;
    }
    a.fd = fd;
    snprintf(a.name, sizeof a.name, "%s", soname);
    uint32_t st = vms_kif_kfe(&a);
    close(fd);
    if (!(st & 1)) {
        report_failure(replace ? "REPLACE" : "CREATE", argv[0], st);
        return 1;
    }
    return 0;
}

static int cmd_add(int argc, char *argv[])     { return add_or_replace(0, argc, argv); }
static int cmd_replace(int argc, char *argv[]) { return add_or_replace(1, argc, argv); }

static int cmd_remove(int argc, char *argv[])
{
    if (argc < 1) {
        msg_printf("%%INSTALL-E-NOFILE, no image filespec specified");
        return 1;
    }
    char soname[64] = {0}, path[256] = {0};
    if (resolve_filespec(argv[0], soname, sizeof soname, path, sizeof path) != 0) {
        msg_printf("%%INSTALL-E-BADFILE, cannot resolve \"%s\"", argv[0]);
        return 1;
    }
    struct vms_kfe_args a;
    memset(&a, 0, sizeof a);
    a.op = VMS_KFE_OP_REMOVE;
    a.fd = -1;                       /* by name: the file may be gone */
    snprintf(a.name, sizeof a.name, "%s", soname);
    uint32_t st = vms_kif_kfe(&a);
    if (!(st & 1)) {
        report_failure("REMOVE", argv[0], st);
        return 1;
    }
    return 0;
}

static void print_entry(const struct vms_kfe_args *e, int full)
{
    out_printf("   %-30s %s%s%s%s", e->name,
               (e->flags & VMS_KFE_F_OPEN) ? "Open " : "",
               (e->flags & VMS_KFE_F_HDRRES) ? "Hdr " : "",
               (e->flags & VMS_KFE_F_SHARED) ? "Shar " : "",
               (e->flags & VMS_KFE_F_PRIV) ? "Prv " : "");
    if (!full)
        return;
    out_printf("        Entry access count         = %u", (unsigned)e->access);
    if (e->flags & VMS_KFE_F_PRIV) {
        char line[512];
        size_t n = (size_t)snprintf(line, sizeof line, "        Privileges =");
        for (size_t k = 0; k < sizeof priv_names / sizeof priv_names[0]; k++)
            if ((e->privs & priv_names[k].bit) && n < sizeof line)
                n += (size_t)snprintf(line + n, sizeof line - n, " %s", priv_names[k].name);
        if (n < sizeof line)
            snprintf(line + n, sizeof line - n, " ");
        ppf_put(0, line);
    }
    ppf_put(0, "");
}

static int cmd_list(int argc, char *argv[])
{
    int full = 0;
    const char *only = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcasecmp(argv[i], "/FULL") == 0)
            full = 1;
        else if (argv[i][0] != '/' && !only)
            only = argv[i];
        else {
            msg_printf("%%INSTALL-E-IVQUAL, unrecognized qualifier \"%s\"", argv[i]);
            return 1;
        }
    }
    struct vms_kfe_args a;
    if (only) {
        char soname[64] = {0}, path[256] = {0};
        if (resolve_filespec(only, soname, sizeof soname, path, sizeof path) != 0) {
            msg_printf("%%INSTALL-E-BADFILE, cannot resolve \"%s\"", only);
            return 1;
        }
        memset(&a, 0, sizeof a);
        a.op = VMS_KFE_OP_FIND_NAME;
        a.fd = -1;
        snprintf(a.name, sizeof a.name, "%s", soname);
        uint32_t st = vms_kif_kfe(&a);
        if (!(st & 1)) {
            msg_printf("%%INSTALL-W-FAIL, failed to LIST entry for %s", only);
            msg_printf("-INSTALL-E-NOKFEFND, Known File Entry not found");
            return 1;
        }
        ppf_put(0, "");
        print_entry(&a, full);
        return 0;
    }
    uint32_t idx = 0;
    int n = 0;
    ppf_put(0, "");
    for (int guard = 0; guard < VMS_KFE_MAX; guard++) {
        memset(&a, 0, sizeof a);
        a.op = VMS_KFE_OP_LIST;
        a.fd = -1;
        a.index = idx;
        uint32_t st = vms_kif_kfe(&a);
        if (st == ST_NOSUCHFILE)
            break;                         /* past the last entry */
        if (!(st & 1)) {
            report_failure("LIST", "the known file list", st);
            return 1;
        }
        print_entry(&a, full);
        idx = a.index;
        n++;
    }
    if (n == 0)
        msg_put("%INSTALL-I-NOIMAGES, no images installed");
    return 0;
}

static void usage(void)
{
    out_printf("Usage:");
    out_printf("  INSTALL ADD     <image-filespec> [/OPEN] [/SHARED] [/HEADER_RESIDENT] [/PRIVILEGED=(...)]");
    out_printf("  INSTALL REPLACE <image-filespec> [qualifiers as ADD]");
    out_printf("  INSTALL LIST    [<image-filespec>] [/FULL]");
    out_printf("  INSTALL REMOVE  <image-filespec>");
}

int main(int argc, char *argv[])
{
    ppf_bind();
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *verb = argv[1];
    if (strcasecmp(verb, "ADD") == 0)
        return cmd_add(argc - 2, argv + 2);
    if (strcasecmp(verb, "REPLACE") == 0)
        return cmd_replace(argc - 2, argv + 2);
    if (strcasecmp(verb, "LIST") == 0)
        return cmd_list(argc - 2, argv + 2);
    if (strcasecmp(verb, "REMOVE") == 0)
        return cmd_remove(argc - 2, argv + 2);

    msg_printf("%%INSTALL-E-SYNTAX, unrecognized command \"%s\"", verb);
    usage();
    return 1;
}
