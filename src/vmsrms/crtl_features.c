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
#include <string.h>

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
#else
#define FILE_SHARING_NAME "DECC$FILE_SHARING"
#endif

static struct feature features[OVMX_CRTL_FEAT_COUNT] = {
    [OVMX_CRTL_FEAT_FILE_SHARING - 1] = { FILE_SHARING_NAME, 0, 1, 0, 0 },
};

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
    struct feature *f = by_index(index);

    if (!f) {
        errno = EINVAL;
        return NULL;
    }
    return f->name;
}

int ovmx_crtl_feature_get_value(int index, int mode)
{
    struct feature *f = by_index(index);

    if (!f || (mode != 0 && mode != 1)) {
        errno = EINVAL;
        return -1;
    }
    return mode == 0 ? f->def : f->cur;
}

int ovmx_crtl_feature_set_value(int index, int mode, int value)
{
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
