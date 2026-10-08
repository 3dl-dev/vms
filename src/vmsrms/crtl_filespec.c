/*
 * crtl_filespec.c - the DEC C RTL file-specification translators (vms-32ae):
 * decc$to_vms, decc$from_vms and decc$translate_vms, plus the one internal
 * Unix<->VMS translator the C RTL's own file layer shares.
 *
 * Behaviour follows the public HP C Run-Time Library Reference Manual entries
 * for these routines and its "File Specification Conversion" rules:
 *
 *   UNIX style                    OpenVMS style
 *   /dev/dir1/dir2/file.ext   ->  DEV:[DIR1.DIR2]FILE.EXT
 *   /dev/file.ext             ->  DEV:FILE.EXT      (a device or logical name
 *                                                    followed by the file)
 *   /dev                      ->  DEV:[000000]      (as a directory)
 *   dir1/dir2/file.ext        ->  [.DIR1.DIR2]FILE.EXT
 *   ./file.ext                ->  []FILE.EXT  (written FILE.EXT: same meaning)
 *   ../file.ext               ->  [-]FILE.EXT
 *   dir/                      ->  [.DIR]
 *   file                      ->  FILE.          (an explicit null type, so no
 *                                                  default type is applied)
 *   a.b.c                     ->  A_B.C          (ODS-2 allows one dot: all but
 *                                                  the last become underscores)
 *
 * Case is preserved (RMS upcases on ODS-2). A specification that is already in
 * OpenVMS syntax (it has a ':' '[' '<' or ';' and no '/') passes through
 * unchanged. ".." components are resolved lexically against preceding
 * components (VMS has no symbolic links, so this is exact); a ".." that would
 * climb above an absolute path's device root is an error.
 *
 * The reverse direction (decc$from_vms / decc$translate_vms) maps
 * DEV:[A.B]F.T;V to /DEV/A/B/F.T (the version is dropped, an empty type loses
 * its dot), [.A]F.T to A/F.T, [-]F.T to ../F.T, and an absolute directory with
 * no device to the default device /SYS$DISK/...
 *
 * Wildcards: with allow_wild (decc$to_vms) or wild_flag (decc$from_vms) a spec
 * containing '*' or '%' is expanded by RMS -- SYS$PARSE then SYS$SEARCH over the
 * real directory -- and the action routine is called once per match, in
 * directory order, until it returns 0. That is the only part of this file that
 * needs RMS; it lives in LIBVMSRMS$SHR, so these entry points are carried by
 * the RMS-backed (pass-2) DECC$SHR.
 *
 * Every string handed to an action routine or returned by decc$translate_vms
 * is in heap memory (the C RTL heap is P0, vms-122), so a 32-bit-pointer
 * caller -- the DEC C default, e.g. GCC's VMS-host code -- can address it.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rms/rms.h"
#include "rms/crtl_filespec.h"

/* ------------------------------------------------------------------ util ---- */

int ovmx_crtl_is_vms_syntax(const char *s)
{
    if (!s || strchr(s, '/'))
        return 0;
    return strpbrk(s, ":[]<>;") != NULL;
}

static int has_wild(const char *s)
{
    return strpbrk(s, "*%") != NULL || strstr(s, "...") != NULL;
}

/* Append [p, p+n) to out at *len (bounded); -1 on overflow. */
static int put(char *out, size_t outsz, size_t *len, const char *p, size_t n)
{
    if (*len + n + 1 > outsz)
        return -1;
    memcpy(out + *len, p, n);
    *len += n;
    out[*len] = '\0';
    return 0;
}

static int puts0(char *out, size_t outsz, size_t *len, const char *p)
{
    return put(out, outsz, len, p, strlen(p));
}

/* ODS-2 file name: keep the last '.', turn every earlier one into '_'. A name
 * without a dot gets an explicit null type ("FILE."). */
static int put_name(char *out, size_t outsz, size_t *len, const char *p, size_t n)
{
    const char *last = NULL;
    for (size_t i = 0; i < n; i++)
        if (p[i] == '.')
            last = p + i;
    for (size_t i = 0; i < n; i++) {
        char c = (p[i] == '.' && p + i != last) ? '_' : p[i];
        if (put(out, outsz, len, &c, 1) < 0)
            return -1;
    }
    if (!last && put(out, outsz, len, ".", 1) < 0)
        return -1;
    return 0;
}

/* ----------------------------------------------------------- Unix -> VMS ---- */

#define FS_MAXCOMP 64

int ovmx_crtl_unix_to_vms(const char *in, char *out, size_t outsz, int mode)
{
    if (!in || !*in || !out || outsz == 0) {
        errno = EINVAL;
        return -1;
    }
    if (ovmx_crtl_is_vms_syntax(in)) {
        size_t n = strlen(in);
        if (n + 1 > outsz) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(out, in, n + 1);
        return OVMX_FS_PASSTHRU;
    }

    int absolute = in[0] == '/';
    size_t inlen = strlen(in);
    int trailing = in[inlen - 1] == '/';

    /* Split into components, dropping empty and "." ones and folding "..". */
    const char *cp[FS_MAXCOMP];
    size_t cl[FS_MAXCOMP];
    int nc = 0;
    int ups = 0;                   /* leading ".." of a relative path */
    int last_dot = 0;              /* the path ended in "." or ".." */
    const char *p = in;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *s = p;
        while (*p && *p != '/')
            p++;
        size_t n = (size_t)(p - s);
        last_dot = 0;
        if (n == 1 && s[0] == '.') {
            last_dot = 1;
            continue;
        }
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            last_dot = 1;
            /* The device of an absolute path is component 0; never pop it. */
            if (nc > (absolute ? 1 : 0)) {
                nc--;
            } else if (absolute) {
                errno = EINVAL;    /* above the device root */
                return -1;
            } else {
                ups++;
            }
            continue;
        }
        if (nc == FS_MAXCOMP) {
            errno = ENAMETOOLONG;
            return -1;
        }
        cp[nc] = s;
        cl[nc] = n;
        nc++;
    }

    int as_dir;
    if (mode == OVMX_FS_DIR)
        as_dir = 1;
    else if (mode == OVMX_FS_FILE)
        as_dir = 0;
    else
        as_dir = trailing || last_dot;
    if (mode == OVMX_FS_FILE && (trailing || last_dot)) {
        errno = EISDIR;            /* no_directory=1 but only a directory named */
        return -1;
    }

    size_t len = 0;
    out[0] = '\0';
    int first_dir = 0;             /* index of the first directory component */
    if (absolute) {
        if (nc == 0) {
            errno = EINVAL;        /* "/" names no device */
            return -1;
        }
        if (put(out, outsz, &len, cp[0], cl[0]) < 0 || puts0(out, outsz, &len, ":") < 0)
            goto toolong;
        first_dir = 1;
    }

    int ndir = as_dir ? nc - first_dir : nc - first_dir - 1;
    if (ndir < 0)
        ndir = 0;

    if (absolute) {
        if (ndir > 0) {
            if (puts0(out, outsz, &len, "[") < 0)
                goto toolong;
            for (int i = 0; i < ndir; i++) {
                if ((i && puts0(out, outsz, &len, ".") < 0) ||
                    put(out, outsz, &len, cp[first_dir + i], cl[first_dir + i]) < 0)
                    goto toolong;
            }
            if (puts0(out, outsz, &len, "]") < 0)
                goto toolong;
        } else if (as_dir) {
            if (puts0(out, outsz, &len, "[000000]") < 0)
                goto toolong;
        }
    } else if (ndir > 0 || ups > 0) {
        if (puts0(out, outsz, &len, "[") < 0)
            goto toolong;
        int any = 0;
        for (int i = 0; i < ups; i++) {
            if ((any && puts0(out, outsz, &len, ".") < 0) ||
                puts0(out, outsz, &len, "-") < 0)
                goto toolong;
            any = 1;
        }
        for (int i = 0; i < ndir; i++) {
            if (puts0(out, outsz, &len, (any || i == 0) ? "." : "") < 0 ||
                put(out, outsz, &len, cp[i], cl[i]) < 0)
                goto toolong;
            any = 1;
        }
        if (puts0(out, outsz, &len, "]") < 0)
            goto toolong;
    } else if (as_dir) {
        if (puts0(out, outsz, &len, "[]") < 0)
            goto toolong;
    }

    if (!as_dir) {
        int ni = nc - 1;
        if (ni < first_dir) {
            errno = EINVAL;        /* no file name component */
            return -1;
        }
        if (put_name(out, outsz, &len, cp[ni], cl[ni]) < 0)
            goto toolong;
        return OVMX_FS_FILE;
    }
    return OVMX_FS_DIR;

toolong:
    errno = ENAMETOOLONG;
    return -1;
}

/* ----------------------------------------------------------- VMS -> Unix ---- */

int ovmx_crtl_vms_to_unix(const char *in, char *out, size_t outsz)
{
    if (!in || !*in || !out || outsz == 0) {
        errno = EINVAL;
        return -1;
    }
    if (!ovmx_crtl_is_vms_syntax(in) && strchr(in, '/')) {
        size_t n = strlen(in);     /* already UNIX syntax */
        if (n + 1 > outsz) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(out, in, n + 1);
        return 0;
    }

    const char *p = in;
    const char *dev = NULL;
    size_t devlen = 0;
    const char *colon = strchr(p, ':');
    const char *lb = strpbrk(p, "[<");
    if (colon && (!lb || colon < lb)) {
        dev = p;
        devlen = (size_t)(colon - p);
        p = colon + 1;
        if (*p == ':')             /* NODE:: is not a local file */
            goto inval;
    }
    const char *dir = NULL;
    size_t dirlen = 0;
    if (*p == '[' || *p == '<') {
        char close = *p == '[' ? ']' : '>';
        const char *e = strchr(p, close);
        if (!e)
            goto inval;
        dir = p + 1;
        dirlen = (size_t)(e - dir);
        p = e + 1;
    }
    /* name.type, version dropped (';' or a second '.') */
    const char *nm = p;
    const char *semi = strchr(nm, ';');
    size_t nmlen = semi ? (size_t)(semi - nm) : strlen(nm);
    const char *dot = memchr(nm, '.', nmlen);
    if (dot) {
        const char *dot2 = memchr(dot + 1, '.', nmlen - (size_t)(dot + 1 - nm));
        if (dot2)
            nmlen = (size_t)(dot2 - nm);   /* NAME.TYP.VER form */
        if (nmlen == (size_t)(dot - nm) + 1)
            nmlen--;                       /* "NAME." -> "NAME" */
    }

    size_t len = 0;
    out[0] = '\0';
    int relative = 0;
    if (dir && dirlen > 0 && (dir[0] == '.' || dir[0] == '-'))
        relative = 1;
    if (dev) {
        if (puts0(out, outsz, &len, "/") < 0 || put(out, outsz, &len, dev, devlen) < 0)
            goto toolong;
    } else if (dir && !relative && dirlen > 0) {
        if (puts0(out, outsz, &len, "/sys$disk") < 0)
            goto toolong;
    }

    int wrote_dir = 0;
    if (dir && dirlen > 0) {
        const char *d = dir;
        const char *de = dir + dirlen;
        if (*d == '.')
            d++;                                /* relative "[.A.B]" */
        while (d < de) {
            const char *s = d;
            while (d < de && *d != '.')
                d++;
            size_t n = (size_t)(d - s);
            size_t k = 0;
            while (k < n && s[k] == '-')
                k++;
            if (n > 0 && k == n) {              /* "-" / "--": parent levels */
                for (size_t i = 0; i < n; i++) {
                    if ((len && puts0(out, outsz, &len, "/") < 0) ||
                        puts0(out, outsz, &len, "..") < 0)
                        goto toolong;
                }
                wrote_dir = 1;
            } else if (n > 0 && !(n == 6 && memcmp(s, "000000", 6) == 0)) {
                if ((len && puts0(out, outsz, &len, "/") < 0) ||
                    put(out, outsz, &len, s, n) < 0)
                    goto toolong;
                wrote_dir = 1;
            }
            if (d < de)
                d++;                            /* skip the '.' */
        }
    }

    if (nmlen > 0) {
        if ((len && puts0(out, outsz, &len, "/") < 0) ||
            put(out, outsz, &len, nm, nmlen) < 0)
            goto toolong;
    } else if (!len) {
        if (puts0(out, outsz, &len, wrote_dir ? "" : ".") < 0)
            goto toolong;
    }
    return 0;

inval:
    errno = EINVAL;
    return -1;
toolong:
    errno = ENAMETOOLONG;
    return -1;
}

/* ------------------------------------------------------- RMS wildcard walk -- */

typedef int (*fs_each_fn)(const char *vms_spec, void *ctx);

/* SYS$PARSE + SYS$SEARCH over `pattern`, calling each(rsa) per match until it
 * returns 0. Returns the number of calls made, or -1 if the parse failed. */
static int fs_search(const char *pattern, fs_each_fn each, void *ctx)
{
    struct FAB fab = cc$rms_fab;
    struct NAM nam = cc$rms_nam;
    char *esa = malloc(256), *rsa = malloc(256);
    if (!esa || !rsa) {
        free(esa);
        free(rsa);
        errno = ENOMEM;
        return -1;
    }
    fab.fab$l_fna = (char *)pattern;
    fab.fab$b_fns = (uint8_t)strlen(pattern);
    fab.fab$l_nam = &nam;
    nam.nam$l_esa = esa;
    nam.nam$b_ess = 255;
    nam.nam$l_rsa = rsa;
    nam.nam$b_rss = 255;

    int calls = -1;
    if (sys$parse(&fab, 0, 0) & 1) {
        calls = 0;
        while (sys$search(&fab, 0, 0) & 1) {
            rsa[nam.nam$b_rsl] = '\0';
            calls++;
            if (!each(rsa, ctx))
                break;
        }
        rms_search_end(&nam);
    } else {
        errno = ENOENT;
    }
    free(esa);
    free(rsa);
    return calls;
}

/* ---------------------------------------------------------- decc$ entries -- */

typedef int (*to_vms_action)(char *vms_spec, int type);
typedef int (*from_vms_action)(char *unix_spec);

int ovmx_decc_to_vms(const char *spec, to_vms_action action, int allow_wild,
                     int no_directory) __asm__("decc$to_vms");
int ovmx_decc_from_vms(const char *spec, from_vms_action action, int wild_flag)
    __asm__("decc$from_vms");
char *ovmx_decc_translate_vms(const char *spec) __asm__("decc$translate_vms");

struct to_vms_ctx { to_vms_action action; };

static int to_vms_each(const char *vms_spec, void *vctx)
{
    struct to_vms_ctx *c = vctx;
    size_t n = strlen(vms_spec);
    char *copy = malloc(n + 1);
    if (!copy)
        return 0;
    memcpy(copy, vms_spec, n + 1);
    int more = c->action(copy, DECC$K_FILE);
    free(copy);
    return more;
}

int ovmx_decc_to_vms(const char *spec, to_vms_action action, int allow_wild,
                     int no_directory)
{
    if (!spec || !action) {
        errno = EINVAL;
        return 0;
    }
    int mode = no_directory == 1 ? OVMX_FS_FILE
             : no_directory == 2 ? OVMX_FS_DIR : OVMX_FS_AUTO;
    size_t cap = strlen(spec) * 2 + 32;
    char *vms = malloc(cap);
    if (!vms) {
        errno = ENOMEM;
        return 0;
    }
    int kind = ovmx_crtl_unix_to_vms(spec, vms, cap, mode);
    if (kind < 0) {
        free(vms);
        return 0;
    }
    int count;
    if (allow_wild && has_wild(vms)) {
        struct to_vms_ctx c = { action };
        count = fs_search(vms, to_vms_each, &c);
        if (count < 0)
            count = 0;
    } else {
        char last = vms[strlen(vms) - 1];
        int type = (kind == OVMX_FS_DIR ||
                    (kind == OVMX_FS_PASSTHRU &&
                     (last == ']' || last == '>' || last == ':')))
                       ? DECC$K_DIRECTORY : DECC$K_FILE;
        action(vms, type);
        count = 1;
    }
    free(vms);
    return count;
}

struct from_vms_ctx { from_vms_action action; };

static int from_vms_each(const char *vms_spec, void *vctx)
{
    struct from_vms_ctx *c = vctx;
    size_t cap = strlen(vms_spec) + 32;
    char *ux = malloc(cap);
    if (!ux || ovmx_crtl_vms_to_unix(vms_spec, ux, cap) < 0) {
        free(ux);
        return 0;
    }
    int more = c->action(ux);
    free(ux);
    return more;
}

int ovmx_decc_from_vms(const char *spec, from_vms_action action, int wild_flag)
{
    if (!spec || !action) {
        errno = EINVAL;
        return 0;
    }
    struct from_vms_ctx c = { action };
    if (wild_flag && has_wild(spec)) {
        int n = fs_search(spec, from_vms_each, &c);
        return n < 0 ? 0 : n;
    }
    size_t cap = strlen(spec) + 32;
    char *ux = malloc(cap);
    if (!ux) {
        errno = ENOMEM;
        return 0;
    }
    if (ovmx_crtl_vms_to_unix(spec, ux, cap) < 0) {
        free(ux);
        return 0;
    }
    action(ux);
    free(ux);
    return 1;
}

char *ovmx_decc_translate_vms(const char *spec)
{
    static char *buf;              /* one per process (DECC$SHR is non-TLS) */
    static size_t bufsz;
    size_t need = (spec ? strlen(spec) : 0) + 32;
    if (!spec) {
        errno = EINVAL;
        return NULL;
    }
    if (need > bufsz) {
        char *nb = realloc(buf, need);
        if (!nb) {
            errno = ENOMEM;
            return NULL;
        }
        buf = nb;
        bufsz = need;
    }
    if (ovmx_crtl_vms_to_unix(spec, buf, bufsz) < 0)
        return NULL;
    return buf;
}
