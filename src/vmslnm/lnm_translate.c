/*
 * lnm_translate.c - Logical Name Translation Logic
 *
 * Implements single-level and iterative logical name translation.
 * VMS logical names can be chained: A -> B -> C -> final_value.
 * Translation iterates until a terminal value or max depth is reached.
 *
 * The LNM$FILE_DEV search list translates through:
 *   process -> job -> group -> system tables in order.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "vms/logical.h"
#include "ssdef.h"
#include "vms_kif.h"

/* Internal table functions from lnm_table.c */
extern lnm_entry_t *lnm_table_lookup(lnm_table_t *table, const char *name);

/* From lnm_client.c */
extern lnm_table_t *lnm_find_table(lnm_manager_t *mgr, const char *table_name);

/* Copy an equivalence value + attributes into the caller's result buffers. */
static uint32_t fill_result(const char *value, uint32_t attr,
                            char *result, size_t result_size,
                            uint16_t *result_length, uint32_t *attributes)
{
    size_t vallen = strlen(value);
    if (vallen >= result_size)
        vallen = result_size - 1;
    memcpy(result, value, vallen);
    result[vallen] = '\0';
    if (result_length)
        *result_length = (uint16_t)vallen;
    if (attributes)
        *attributes = attr;
    return SS$_NORMAL;
}

/* Copy a found entry's index-0 equivalence into the result buffers. */
static uint32_t fill_from_entry(const lnm_entry_t *entry,
                                char *result, size_t result_size,
                                uint16_t *result_length, uint32_t *attributes)
{
    if (!entry || entry->num_translations == 0)
        return SS$_NOLOGNAM;
    return fill_result(entry->translations[0].value, entry->attributes,
                       result, result_size, result_length, attributes);
}

/*
 * translate_executive - Translate a name in an executive-resident table
 * (LNM$SYSTEM, LNM$GROUP or LNM$JOB).
 *
 * vms-96e2 wired LNM$SYSTEM through vms.ko (vms-d37/#193), reached via the
 * read-only mmap arena through vms_kif_lnm_translate -- this is why
 * SYS$UPDATE, DEFINE/SYSTEM'd at boot by one process, resolves in a later
 * login process (@SYS$UPDATE:PARTS_SETUP.COM). vms-aba wires LNM$GROUP and
 * LNM$JOB the same way: vms_kif_lnm_translate() itself resolves each
 * caller's own GROUP/JOB scope key from the executive (never recomputed
 * here), so this one helper serves all three tables.
 *
 * vms_kif_lnm_translate returns 1 (found), 0 (executive present, name absent),
 * or -1 (executive unavailable). At OVMX runtime the executive is always
 * present (PID 1 pins it, Rule 9), so -1 is a host BUILD/TEST-tooling state
 * only.
 *
 * NO FALLBACK (vms-48ab / vms-aba, INV-6): -1 returns SS$_NOSUCHDEV directly
 * for all three tables. SYSTEM USED to have a process-local fallback here so
 * host ctest could still resolve names seeded via the vms-96e2 create-side
 * fallback; that was the silent-per-process-fake shape Rule 9 forbids and it
 * is gone (vms-48ab). GROUP/JOB never had one. This matches sys$trnlnm's
 * already-honest SYSTEM read (src/libvms/syssvc/sys_logical.c). The host
 * tests that need a real executive live in tests/qemu/
 * (test_syssvc_lnm_system.c, test_syssvc_lnm_groupjob.c).
 */
static uint32_t translate_executive(uint32_t table, const char *name,
                                    char *result, size_t result_size,
                                    uint16_t *result_length, uint32_t *attributes)
{
    char equiv[LNM_MAX_VALUE + 1];
    uint32_t attr = 0;
    int r = vms_kif_lnm_translate(table, name, 0, equiv,
                                  sizeof(equiv), NULL, &attr, NULL);
    if (r == 1)
        return fill_result(equiv, attr, result, result_size,
                           result_length, attributes);
    if (r == 0)
        return SS$_NOLOGNAM;      /* executive present, name absent */

    return SS$_NOSUCHDEV;         /* r < 0: executive absent -- no fallback */
}

static uint32_t translate_system(lnm_manager_t *mgr, const char *name,
                                 char *result, size_t result_size,
                                 uint16_t *result_length, uint32_t *attributes)
{
    (void)mgr;  /* no local fallback -- kept in the signature for callers */
    return translate_executive(VMS_LNM_TBL_SYSTEM, name, result, result_size,
                               result_length, attributes);
}

static uint32_t translate_group(const char *name, char *result,
                                size_t result_size, uint16_t *result_length,
                                uint32_t *attributes)
{
    return translate_executive(VMS_LNM_TBL_GROUP, name, result, result_size,
                               result_length, attributes);
}

static uint32_t translate_job(const char *name, char *result,
                              size_t result_size, uint16_t *result_length,
                              uint32_t *attributes)
{
    return translate_executive(VMS_LNM_TBL_JOB, name, result, result_size,
                               result_length, attributes);
}

/*
 * lnm_translate - Look up a logical name in the specified table.
 *
 * If table_name is a search list (LNM$FILE_DEV / LNM$DCL_LOGICAL) or NULL,
 * searches LNM$PROCESS (process-private) then LNM$JOB, LNM$GROUP and
 * LNM$SYSTEM in that order (all three executive-resident, vms-aba). If
 * found, returns the first equivalence string (index 0).
 *
 * @mgr:           Logical name manager
 * @table_name:    Table to search (or LNM$FILE_DEV for search list)
 * @logical_name:  Name to translate
 * @result:        Buffer to receive the equivalence string
 * @result_size:   Size of the result buffer
 * @result_length: Receives the length of the result (may be NULL)
 * @attributes:    Receives the entry attributes (may be NULL)
 *
 * Returns SS$_NORMAL on success, SS$_NOLOGNAM if not found.
 */
uint32_t lnm_translate(lnm_manager_t *mgr, const char *table_name,
                        const char *logical_name, char *result,
                        size_t result_size, uint16_t *result_length,
                        uint32_t *attributes)
{
    if (!mgr || !logical_name || !result || result_size == 0)
        return SS$_BADPARAM;

    /* Validate logical name */
    size_t namelen = strlen(logical_name);
    if (namelen == 0 || namelen > LNM_MAX_NAME)
        return SS$_IVLOGNAM;

    int is_searchlist = (!table_name ||
                         strcasecmp(table_name, LNM_FILE_DEV) == 0 ||
                         strcasecmp(table_name, LNM_DCL_LOGICAL) == 0);
    int is_system = (table_name &&
                     (strcasecmp(table_name, LNM_SYSTEM_TABLE) == 0 ||
                      strcasecmp(table_name, "LNM$SYSTEM_TABLE") == 0 ||
                      strcasecmp(table_name, "SYSTEM") == 0));
    int is_group = (table_name &&
                    (strcasecmp(table_name, LNM_GROUP_TABLE) == 0 ||
                     strcasecmp(table_name, "GROUP") == 0));
    int is_job = (table_name &&
                 (strcasecmp(table_name, LNM_JOB_TABLE) == 0 ||
                  strcasecmp(table_name, "JOB") == 0));

    if (is_system)
        return translate_system(mgr, logical_name, result, result_size,
                                result_length, attributes);
    if (is_group)
        return translate_group(logical_name, result, result_size,
                               result_length, attributes);
    if (is_job)
        return translate_job(logical_name, result, result_size,
                             result_length, attributes);

    if (is_searchlist) {
        uint32_t st;

        /* LNM$PROCESS first -- process-private, unchanged. */
        if (mgr->process_table) {
            lnm_entry_t *entry = lnm_table_lookup(mgr->process_table, logical_name);
            if (entry && entry->num_translations > 0)
                return fill_from_entry(entry, result, result_size,
                                       result_length, attributes);
        }

        /*
         * Then JOB, GROUP, SYSTEM, all executive-resident, in the standard
         * order (vms-aba). A miss (SS$_NOLOGNAM) falls through to the next
         * table; an absent executive (SS$_NOSUCHDEV) is returned right away
         * -- GROUP and SYSTEM would fail identically, so there is no point
         * trying them (INV-6 honest failure, not a partial search).
         */
        st = translate_job(logical_name, result, result_size,
                           result_length, attributes);
        if (st != SS$_NOLOGNAM)
            return st;
        st = translate_group(logical_name, result, result_size,
                             result_length, attributes);
        if (st != SS$_NOLOGNAM)
            return st;
        return translate_system(mgr, logical_name, result, result_size,
                                result_length, attributes);
    }

    /* A specific, non-executive table (LNM$PROCESS or an unrecognised name). */
    lnm_table_t *table = lnm_find_table(mgr, table_name);
    if (!table)
        return SS$_NOLOGTAB;
    return fill_from_entry(lnm_table_lookup(table, logical_name),
                           result, result_size, result_length, attributes);
}

/*
 * lnm_translate_filespec - the ONE filespec-aware iterative logical-name
 * translation driver (vms-240).
 *
 * This replaces the filespec-BLIND iterative translator removed here, which
 * fed the WHOLE input string to lnm_translate() at every hop and used an ad-hoc
 * "stop if it contains '/' or starts with '.'" heuristic to guess when a
 * translation had turned into a path -- and, worse, IGNORED the LNM$M_TERMINAL
 * translation attribute so a terminal logical kept on translating. (That path only
 * appeared to work because the file-side resolver, vmsfs_resolve_device_r(),
 * also ignored LNM$M_TERMINAL; vms-240 makes both honor it.)
 *
 * Semantics, grounded in public VSI OpenVMS documentation:
 *
 *   - Filespec-aware stripping. Only the leading DEVICE field -- the text up
 *     to the first device colon, after an optional "node::" prefix -- is a
 *     candidate for logical-name translation. The directory/name/type/version
 *     tail is preserved verbatim and re-attached to the final device. A bare
 *     string carrying no filespec punctuation is translated whole (the plain
 *     A -> B -> C logical chain).
 *
 *   - LNM$M_TERMINAL is honored. A translation carrying LNM_ATTR_TERMINAL is
 *     the FINAL equivalence: iteration stops and the value is NOT re-fed as a
 *     further logical name (VSI OpenVMS System Services Reference, $TRNLNM
 *     item code LNM$_ATTRIBUTES / mask LNM$M_TERMINAL; VSI OpenVMS User's
 *     Manual, "Logical Name Translation" -- iterative translation stops at a
 *     terminal name).
 *
 *   - LNM$M_CONCEALED is honored. A concealed device logical NAMES a device
 *     and is kept in the file specification rather than replaced by its
 *     equivalence (VSI OpenVMS User's Manual, "Concealed-Device Logical
 *     Names"). Concealment ends device substitution exactly like a terminal
 *     translation, and the concealed NAME -- not its equivalence -- stays in
 *     the result. (Rooted-directory COMPOSITION of a concealed device is a
 *     filespec-level concern handled in src/vmsfs; this driver only decides
 *     where device substitution stops.)
 *
 *   - Only a device-shaped equivalence substitutes. A hop that resolves to a
 *     directory-bearing spec ("dev:[dir]") is left to the filespec layer, not
 *     folded into the device field here; iteration stops keeping the last
 *     bare device name. A "dev:" equivalence (no directory) substitutes.
 *
 *   - Depth-guarded (LNM_MAX_DEPTH) against a translation loop.
 *
 * @table_name:    table/search-list to translate through (normally
 *                 LNM$FILE_DEV); passed straight to lnm_translate().
 * @attributes:    if non-NULL, receives the attributes of the translation
 *                 that ended the chain (0 if nothing translated).
 *
 * `result` always receives a usable filespec: the fully substituted spec, or
 * the original input when nothing translated. Returns SS$_NORMAL, SS$_BADPARAM
 * for a bad argument, or SS$_RESULTOVF if LNM_MAX_DEPTH is exceeded (result
 * still holds the best-effort partial substitution).
 */
static void lnm_copy_out(const char *s, char *result, size_t result_size,
                         uint16_t *result_length)
{
    size_t l = strlen(s);
    if (l >= result_size)
        l = result_size - 1;
    memcpy(result, s, l);
    result[l] = '\0';
    if (result_length)
        *result_length = (uint16_t)l;
}

uint32_t lnm_translate_filespec(lnm_manager_t *mgr, const char *table_name,
                                const char *filespec, char *result,
                                size_t result_size, uint16_t *result_length,
                                uint32_t *attributes)
{
    if (attributes)
        *attributes = 0;
    if (result_length)
        *result_length = 0;
    if (!mgr || !filespec || !result || result_size == 0)
        return SS$_BADPARAM;

    /* Optional "node::" prefix -- preserved, never translated. */
    char node_prefix[LNM_MAX_VALUE + 1];
    node_prefix[0] = '\0';
    const char *scan = filespec;
    const char *dcolon = strstr(filespec, "::");
    if (dcolon) {
        size_t np = (size_t)(dcolon - filespec) + 2;   /* include "::" */
        if (np >= sizeof(node_prefix))
            np = sizeof(node_prefix) - 1;
        memcpy(node_prefix, filespec, np);
        node_prefix[np] = '\0';
        scan = dcolon + 2;
    }

    /* Device field = text up to the first device colon in `scan`. */
    char devname[LNM_MAX_VALUE + 1];
    const char *tail = "";       /* everything after the device colon */
    int had_colon = 0;
    const char *colon = strchr(scan, ':');
    if (colon) {
        size_t dn = (size_t)(colon - scan);
        if (dn >= sizeof(devname))
            dn = sizeof(devname) - 1;
        memcpy(devname, scan, dn);
        devname[dn] = '\0';
        tail = colon + 1;
        had_colon = 1;
    } else {
        /* No device colon: a filespec with directory/name/type punctuation
         * has no device to translate -- return it unchanged. A bare token is
         * translated whole (the A -> B -> C chain). */
        if (strpbrk(scan, "[]<>.;/") != NULL) {
            lnm_copy_out(filespec, result, result_size, result_length);
            return SS$_NORMAL;
        }
        strncpy(devname, scan, sizeof(devname) - 1);
        devname[sizeof(devname) - 1] = '\0';
    }

    if (devname[0] == '\0') {
        lnm_copy_out(filespec, result, result_size, result_length);
        return SS$_NORMAL;
    }

    char current[LNM_MAX_VALUE + 1];
    strncpy(current, devname, sizeof(current) - 1);
    current[sizeof(current) - 1] = '\0';

    uint32_t last_attr = 0;
    int overflow = 0;
    for (int depth = 0; ; depth++) {
        if (depth >= LNM_MAX_DEPTH) {
            overflow = 1;
            break;
        }

        char next[LNM_MAX_VALUE + 1];
        uint16_t nlen = 0;
        uint32_t attr = 0;
        uint32_t st = lnm_translate(mgr, table_name, current, next,
                                    sizeof(next), &nlen, &attr);
        if (st != SS$_NORMAL && st != SS$_SUPERSEDE)
            break;               /* current is not a logical -> final device */
        if (nlen >= sizeof(next))
            nlen = sizeof(next) - 1;
        next[nlen] = '\0';
        last_attr = attr;

        if (attr & LNM_ATTR_CONCEALED)
            break;               /* concealed: keep the NAME (current) */

        if (strpbrk(next, "[]<>") != NULL) {
            /* Directory-bearing equivalence: not a plain device
             * substitution -- leave the device to the filespec layer and stop
             * with the last bare device name in `current`. */
            break;
        }

        strncpy(current, next, sizeof(current) - 1);
        current[sizeof(current) - 1] = '\0';

        if (attr & LNM_ATTR_TERMINAL)
            break;               /* terminal: `next` is final, no re-translate */
        if (strchr(current, ':') != NULL)
            break;               /* "dev:" concrete device -> stop */
    }

    if (attributes)
        *attributes = last_attr;

    /* Reassemble node_prefix + final-device + tail. `current` is a bare name
     * or a "dev:" form (never directory-bearing -- we stop before adopting
     * one), so normalise to exactly one device colon before the tail. */
    char out[LNM_MAX_VALUE * 2 + 8];
    if (had_colon) {
        size_t cl = strlen(current);
        if (cl > 0 && current[cl - 1] == ':')
            current[cl - 1] = '\0';    /* "DKA100:" -> "DKA100" */
        snprintf(out, sizeof(out), "%s%s:%s", node_prefix, current, tail);
    } else {
        snprintf(out, sizeof(out), "%s%s", node_prefix, current);
    }

    lnm_copy_out(out, result, result_size, result_length);
    return overflow ? SS$_RESULTOVF : SS$_NORMAL;
}

/*
 * lnm_translate_values - vms-420: the multi-value counterpart of
 * lnm_translate(). A SPECIFIC table only (never the LNM$FILE_DEV /
 * LNM$DCL_LOGICAL search-list alias -- a caller wanting the search-list
 * default's ALL values would need to pick which table's values to show,
 * exactly as lnm_translate()'s search-list branch picks which table's
 * single value to show; SHOW LOGICAL's callers already loop tables
 * themselves for that reason).
 *
 * SYSTEM/GROUP/JOB read the executive arena by walking vms_kif_lnm_translate
 * with an increasing index (0, 1, 2, ...) until it reports the entry's
 * num_equiv reached -- the same "no fallback" honesty as lnm_translate()'s
 * translate_executive(): an unavailable executive returns SS$_NOSUCHDEV
 * directly (CLAUDE.md Rule 9 / INV-6), never a partial or fabricated result.
 * LNM$PROCESS_TABLE (and any other local table) already stores every
 * equivalence string in entry->translations[] (lnm_create_multi), so those
 * are copied out directly under no lock beyond the table's own (none is
 * currently taken for reads elsewhere in this file either).
 */
uint32_t lnm_translate_values(lnm_manager_t *mgr, const char *table_name,
                               const char *logical_name,
                               char values[][LNM_MAX_VALUE + 1],
                               uint8_t max_values, uint8_t *num_values,
                               uint32_t *attributes)
{
    if (num_values)
        *num_values = 0;

    if (!mgr || !logical_name || !values || max_values == 0)
        return SS$_BADPARAM;

    size_t namelen = strlen(logical_name);
    if (namelen == 0 || namelen > LNM_MAX_NAME)
        return SS$_IVLOGNAM;

    int is_system = (table_name &&
                     (strcasecmp(table_name, LNM_SYSTEM_TABLE) == 0 ||
                      strcasecmp(table_name, "LNM$SYSTEM_TABLE") == 0 ||
                      strcasecmp(table_name, "SYSTEM") == 0));
    int is_group = (table_name &&
                    (strcasecmp(table_name, LNM_GROUP_TABLE) == 0 ||
                     strcasecmp(table_name, "GROUP") == 0));
    int is_job = (table_name &&
                 (strcasecmp(table_name, LNM_JOB_TABLE) == 0 ||
                  strcasecmp(table_name, "JOB") == 0));

    if (is_system || is_group || is_job) {
        uint32_t exec_tbl = is_system ? VMS_LNM_TBL_SYSTEM
                          : is_group  ? VMS_LNM_TBL_GROUP
                                      : VMS_LNM_TBL_JOB;
        uint8_t idx, nequiv = 0, filled = 0;
        uint32_t attr = 0;

        for (idx = 0; ; idx++) {
            char equiv[LNM_MAX_VALUE + 1];
            uint8_t this_nequiv = 0;
            int r = vms_kif_lnm_translate(exec_tbl, logical_name, idx, equiv,
                                          sizeof(equiv), NULL, &attr,
                                          &this_nequiv);
            if (r < 0)
                return SS$_NOSUCHDEV;        /* executive absent -- no fallback */
            if (idx == 0 && this_nequiv == 0)
                return SS$_NOLOGNAM;         /* name not present at all */
            nequiv = this_nequiv;
            if (!r)
                break;                       /* idx reached the end of the list */
            if (filled < max_values) {
                strncpy(values[filled], equiv, LNM_MAX_VALUE);
                values[filled][LNM_MAX_VALUE] = '\0';
                filled++;
            }
            if (idx + 1 >= nequiv)
                break;
        }

        if (num_values)
            *num_values = filled;
        if (attributes)
            *attributes = attr;
        return SS$_NORMAL;
    }

    /* A local table (LNM$PROCESS_TABLE or an unrecognised name). */
    lnm_table_t *table = lnm_find_table(mgr, table_name);
    if (!table)
        return SS$_NOLOGTAB;

    lnm_entry_t *entry = lnm_table_lookup(table, logical_name);
    if (!entry || entry->num_translations == 0)
        return SS$_NOLOGNAM;

    uint8_t n = entry->num_translations;
    if (n > max_values)
        n = max_values;
    for (uint8_t i = 0; i < n; i++) {
        strncpy(values[i], entry->translations[i].value, LNM_MAX_VALUE);
        values[i][LNM_MAX_VALUE] = '\0';
    }
    if (num_values)
        *num_values = n;
    if (attributes)
        *attributes = entry->attributes;
    return SS$_NORMAL;
}

/*
 * lnm_translate_searchlist - all equivalence strings for a name, resolved in
 * the LNM$FILE_DEV table search order. See the header for the contract.
 *
 * This is lnm_translate()'s search-list branch generalised to return every
 * value of the first matching table, reusing lnm_translate_values() per
 * table so both share one arena-read implementation. The first table that
 * has the name wins outright -- VMS does not merge equivalence lists across
 * tables, so neither do we (a PROCESS-table definition SHADOWS a SYSTEM one
 * of the same name completely, not element-wise).
 */
uint32_t lnm_translate_searchlist(lnm_manager_t *mgr, const char *logical_name,
                                  char values[][LNM_MAX_VALUE + 1],
                                  uint8_t max_values, uint8_t *num_values,
                                  uint32_t *attributes)
{
    if (num_values)
        *num_values = 0;

    if (!mgr || !logical_name || !values || max_values == 0)
        return SS$_BADPARAM;

    static const char *order[] = {
        LNM_PROCESS_TABLE, LNM_JOB_TABLE, LNM_GROUP_TABLE, LNM_SYSTEM_TABLE
    };

    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        uint8_t n = 0;
        uint32_t st = lnm_translate_values(mgr, order[i], logical_name,
                                           values, max_values, &n, attributes);
        if (st == SS$_NORMAL) {
            if (num_values)
                *num_values = n;
            return SS$_NORMAL;
        }
        /*
         * SS$_NOSUCHDEV means an executive table had to be read but /dev/vms
         * is unreachable -- report it honestly rather than silently skipping
         * to a table that would fail identically (INV-6). SS$_NOLOGNAM /
         * SS$_NOLOGTAB just mean "not here": fall through to the next table.
         */
        if (st == SS$_NOSUCHDEV)
            return SS$_NOSUCHDEV;
    }

    return SS$_NOLOGNAM;
}
