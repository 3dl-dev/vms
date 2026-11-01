/*
 * crtl_features.c - the DEC C RTL feature-switch API (vms-db7): decc$feature_get_index,
 * decc$feature_get_name, decc$feature_get_value, decc$feature_set_value, and the
 * decc$set_reentrancy / decc$get_reentrancy pair, over a table that holds ONLY the
 * features OVMX's C RTL actually honours.
 *
 * THE HONEST RULE. On OpenVMS an unknown feature name answers -1 with errno EINVAL; it is
 * never accepted and then ignored. This table therefore lists a feature only when some
 * code path really changes behaviour with it:
 *
 *   DECC$FILE_SHARING (0..1, default 0)  the C RTL file layer (crtl_rms_fd.c) opens its
 *       FAB with SHRGET|SHRPUT instead of no sharing, so a second fopen of a file
 *       already open for write succeeds (RMS CW lock) instead of failing on the
 *       Files-11 file-access lock (RMS EX lock).
 *   DECC$EFS_CHARSET (0..1, default 0)  the UNIX<->OpenVMS name translator
 *       (crtl_filespec.c: decc$to_vms, decc$translate_vms, and the file layer's own
 *       translation) writes and reads extended (ODS-5) names with ^ escapes; off, a
 *       UNIX name that ODS-2 cannot carry is refused (rd vms-da9b, observed on
 *       OpenVMS Alpha V8.4: tests/lab/captures/ods5-alpha-20261022 session 5).
 *   DECC$EFS_CASE_PRESERVE (0..1, default 0)  the same translator keeps case; off,
 *       it upper-cases toward OpenVMS and lower-cases toward UNIX (same capture).
 *
 * Each switch takes its first value from the logical name of the same name
 * ("ENABLE" / "DISABLE", or a number in range), read once, before the first
 * query or set -- the way the DEC C RTL reads its feature logicals when the
 * image starts.
 *
 * A name the table does not carry is -1/EINVAL even when DEC C knows it: asking for a
 * switch OVMX does not implement must not look like success. Programs that set a list of
 * switches and skip the ones that return -1 (the idiom in the corpus' ipc_benchmark) work
 * unchanged.
 *
 * Index 0 is never valid (callers test `index > 0`); the first feature is index 1.
 * Modes follow the DEC C documentation: 0 = the default value, 1 = the current value.
 * A value outside the feature's range is -1/EINVAL.
 *
 * Reentrancy: OVMX's C RTL (musl) is always thread-safe, so C$C_MULTITHREAD is honoured
 * trivially; the level is recorded and read back by decc$get_reentrancy. A level other
 * than the two defined ones is -1/EINVAL.
 *
 * The alpha DECC$SHR binds the decc$ names to these ovmx_crtl_* functions with the
 * symbol-vector alias form (mk_decc_shr.sh); they are plain
 * names here so the cc1's decc$ auto-decoration never touches them.
 */
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "rms/crtl_features.h"

struct feature {
    const char *name;
    int min, max;
    int def;        /* default value (mode 0) */
    int cur;        /* current value (mode 1) */
};

/*
 * OVMX_CRTL_NO_RMS_VENEER: built into a C RTL that does NOT open files through RMS
 * (the x86_64/aarch64 DECC$SHR, musl over the substrate's file descriptors), where no
 * feature in this table changes anything -- so none is offered: every name is -1/EINVAL
 * there, and the API still answers as DEC C does for a switch the RTL does not have.
 */
#if defined(OVMX_CRTL_NO_RMS_VENEER)
#define FILE_SHARING_NAME NULL
#define EFS_CHARSET_NAME NULL
#define EFS_CASE_PRESERVE_NAME NULL
#else
#define FILE_SHARING_NAME "DECC$FILE_SHARING"
#define EFS_CHARSET_NAME "DECC$EFS_CHARSET"
#define EFS_CASE_PRESERVE_NAME "DECC$EFS_CASE_PRESERVE"
#endif

static struct feature features[OVMX_CRTL_FEAT_COUNT] = {
    [OVMX_CRTL_FEAT_FILE_SHARING - 1] = { FILE_SHARING_NAME, 0, 1, 0, 0 },
    [OVMX_CRTL_FEAT_EFS_CHARSET - 1] = { EFS_CHARSET_NAME, 0, 1, 0, 0 },
    [OVMX_CRTL_FEAT_EFS_CASE_PRESERVE - 1] = { EFS_CASE_PRESERVE_NAME, 0, 1, 0, 0 },
};

#if !defined(OVMX_CRTL_NO_RMS_VENEER)
#include <stdint.h>
#include "descrip.h"
#include "starlet.h"
#include "lnmdef.h"

/* The feature's logical name, if defined: "ENABLE" -> 1, "DISABLE" -> 0, a
 * decimal number as itself; anything else (or no such name) leaves it alone. */
static void feature_from_logical(struct feature *f)
{
    static const char tab[] = "LNM$FILE_DEV";
    struct dsc$descriptor_s dtab = { sizeof tab - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)tab };
    struct dsc$descriptor_s dnam = { 0, DSC$K_DTYPE_T, DSC$K_CLASS_S, NULL };
    char buf[64];
    uint16_t rl = 0;
    struct item_list_3 il[2];
    int v = -1;

    memset(il, 0, sizeof il);
    il[0].item_code = LNM$_STRING;
    il[0].buflen = sizeof buf - 1;
    il[0].bufaddr = buf;
    il[0].retlen = &rl;
    dnam.dsc$w_length = (uint16_t)strlen(f->name);
    dnam.dsc$a_pointer = (char *)f->name;
    if (!(sys$trnlnm(NULL, &dtab, &dnam, NULL, il) & 1))
        return;
    buf[rl < sizeof buf ? rl : sizeof buf - 1] = '\0';
    if (strcasecmp(buf, "ENABLE") == 0)
        v = 1;
    else if (strcasecmp(buf, "DISABLE") == 0)
        v = 0;
    else if (buf[0] >= '0' && buf[0] <= '9')
        v = atoi(buf);
    if (v >= f->min && v <= f->max)
        f->cur = v;
}
#endif

/* Read the feature logicals once, before the first query or set. */
static void features_init(void)
{
    static int done;
    if (done)
        return;
    done = 1;
#if !defined(OVMX_CRTL_NO_RMS_VENEER)
    for (int i = 0; i < OVMX_CRTL_FEAT_COUNT; i++)
        if (features[i].name)
            feature_from_logical(&features[i]);
#endif
}

static int reentrancy_level = OVMX_C_MULTITHREAD;

static struct feature *by_index(int index)
{
    if (index < 1 || index > OVMX_CRTL_FEAT_COUNT || !features[index - 1].name)
        return NULL;
    return &features[index - 1];
}

int ovmx_crtl_feature_get_index(const char *name)
{
    int i;
    features_init();

    if (!name) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < OVMX_CRTL_FEAT_COUNT; i++)
        if (features[i].name && strcmp(features[i].name, name) == 0)
            return i + 1;
    errno = EINVAL;
    return -1;
}

const char *ovmx_crtl_feature_get_name(int index)
{
    features_init();
    struct feature *f = by_index(index);

    if (!f) {
        errno = EINVAL;
        return NULL;
    }
    return f->name;
}

int ovmx_crtl_feature_get_value(int index, int mode)
{
    features_init();
    struct feature *f = by_index(index);

    if (!f || (mode != 0 && mode != 1)) {
        errno = EINVAL;
        return -1;
    }
    return mode == 0 ? f->def : f->cur;
}

int ovmx_crtl_feature_set_value(int index, int mode, int value)
{
    features_init();
    struct feature *f = by_index(index);

    if (!f || (mode != 0 && mode != 1) || value < f->min || value > f->max) {
        errno = EINVAL;
        return -1;
    }
    if (mode == 0)
        f->def = value;
    else
        f->cur = value;
    return 0;
}

/* The C RTL file layer's query: the CURRENT value of a feature it honours. */
int ovmx_crtl_feature_current(int index)
{
    features_init();
    struct feature *f = by_index(index);

    return f ? f->cur : 0;
}

int ovmx_crtl_set_reentrancy(int level)
{
    if (level != OVMX_C_MULTITHREAD && level != OVMX_C_SINGLE_THREAD) {
        errno = EINVAL;
        return -1;
    }
    reentrancy_level = level;
    return 0;
}

int ovmx_crtl_get_reentrancy(void)
{
    return reentrancy_level;
}
