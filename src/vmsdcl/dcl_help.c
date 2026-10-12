/*
 * dcl_help.c - Hierarchical HELP library engine (vms-01b)
 *
 * Replaces the former printf shim (dcl_cmd_misc.c cmd_help: per-verb one-liner
 * + three hardcoded SHOW/SET/DIRECTORY blocks + a fake one-shot "Topic?") and
 * the orphaned, never-dispatched reader (tools/vms_help.c, which loaded a
 * compiled-in C string) with a single real reader that walks a hierarchy
 * parsed from library DATA -- no hardcoded topic content.
 *
 * Format & wording provenance is documented in dcl/help.h (project Rule 8):
 * the numbered-level ".HLP" source format, the listing headers, the prompt
 * wording, and the not-found message are from public OpenVMS documentation and
 * observed HELP output. The unpublished ".HLB" binary layout is not used; the
 * documented ".HLP" source form is read directly (an OVMX design choice).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#include "dcl/help.h"
#include "dcl/hlb.h"
#include "ssdef.h"

/*
 * vms-4ac: the HELP library (SYS$HELP:HELPLIB.HLP) is read over the Files-11
 * ODS-2 ACP on the product runtime -- the /vms POSIX passthrough it used to
 * fopen() was retired by the atomic flip (epic vms-208), so a runtime HELP
 * answered %HELP-E-OPENIN even though HELPLIB.HLP is mastered on the volume.
 *
 * The RMS/ACP read lives in a SEPARATE translation unit (dcl_help_acp.c),
 * reached here through TWO WEAK SEAMS so this engine stays free of the RMS and
 * vmsfs dependencies -- the hermetic engine unit test (tests/dcl/
 * test_help_engine.c) compiles THIS file alone and links neither. When the
 * seam is present (DCL.EXE, HELP.EXE), a VMS filespec is read over the ACP with
 * a POSIX /vms fallback; when it is absent (the engine test, which only ever
 * feeds Linux temp paths), the VMS-spec branch is simply inert.
 *
 *   help_acp_library_text(spec)          -> malloc'd .HLP text over the ACP, or
 *                                           NULL if the ACP cannot reach it.
 *   help_acp_vms_to_linux(spec,buf,sz)   -> 1 + Linux /vms path for the POSIX
 *                                           fallback, 0 if it cannot translate.
 * The same #pragma weak layering seam rms_textfile.c uses (LIBVMS sits below
 * RMS, so a hard reference would invert the layering).
 */
char *help_acp_library_text(const char *vms_spec) __attribute__((weak));
int   help_acp_vms_to_linux(const char *vms_spec, char *buf, size_t bufsz)
      __attribute__((weak));

/* ------------------------------------------------------------------ */
/* Node construction                                                   */
/* ------------------------------------------------------------------ */

static char *help_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static help_node_t *node_new(int level, const char *name)
{
    help_node_t *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->level = level;
    n->name = help_strdup(name ? name : "");
    if (!n->name) { free(n); return NULL; }
    return n;
}

static void node_add_child(help_node_t *parent, help_node_t *child)
{
    child->parent = parent;
    if (parent->last_child)
        parent->last_child->next_sibling = child;
    else
        parent->first_child = child;
    parent->last_child = child;
}

/* Append one raw body line (without trailing newline) to a node's text. */
static void node_append_text(help_node_t *n, const char *line)
{
    size_t old = n->text ? strlen(n->text) : 0;
    size_t add = strlen(line);
    char *p = realloc(n->text, old + add + 2); /* + '\n' + '\0' */
    if (!p) return;
    memcpy(p + old, line, add);
    p[old + add] = '\n';
    p[old + add + 1] = '\0';
    n->text = p;
}

static void node_free(help_node_t *n)
{
    if (!n) return;
    help_node_t *c = n->first_child;
    while (c) {
        help_node_t *next = c->next_sibling;
        node_free(c);
        c = next;
    }
    free(n->name);
    free(n->text);
    free(n);
}

/* ------------------------------------------------------------------ */
/* Parsing (numbered-level .HLP source)                                */
/* ------------------------------------------------------------------ */

/*
 * A key line is a level digit (1..9) in column 1 immediately followed by a
 * space and the key name (VMS HELP source convention). Any other line is body
 * text for the current key. Blank lines outside a key are ignored.
 */
static void parse_line(help_lib_t *lib, help_node_t **stack, help_node_t **cur,
                       const char *line)
{
    if (line[0] >= '1' && line[0] <= '9' && line[1] == ' ') {
        int level = line[0] - '0';
        const char *name = line + 2;
        while (*name == ' ' || *name == '\t') name++;

        char keybuf[128];
        size_t k = 0;
        /* Key is the first whitespace-delimited token (VMS keys are single
         * tokens; the rest of the line, if any, is ignored as a comment). */
        while (name[k] && name[k] != ' ' && name[k] != '\t' &&
               k + 1 < sizeof(keybuf)) {
            keybuf[k] = name[k];
            k++;
        }
        keybuf[k] = '\0';
        if (keybuf[0] == '\0') return; /* malformed; skip */

        help_node_t *node = node_new(level, keybuf);
        if (!node) return;

        /* Its parent is the most recent node at level-1 on the stack. */
        help_node_t *parent = (level >= 2 && stack[level - 1])
                                  ? stack[level - 1]
                                  : lib->root;
        node_add_child(parent, node);

        stack[level] = node;
        /* Deeper stack slots are stale once we open a shallower key. */
        for (int d = level + 1; d < 10; d++) stack[d] = NULL;
        *cur = node;
    } else if (*cur) {
        node_append_text(*cur, line);
    }
}

static help_lib_t *lib_new(void)
{
    help_lib_t *lib = calloc(1, sizeof(*lib));
    if (!lib) return NULL;
    lib->root = node_new(0, "");
    if (!lib->root) { free(lib); return NULL; }
    return lib;
}

help_lib_t *help_open_text(const char *text)
{
    if (!text) return NULL;
    help_lib_t *lib = lib_new();
    if (!lib) return NULL;

    help_node_t *stack[10] = {0};
    help_node_t *cur = NULL;

    const char *p = text;
    char line[1024];
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        /* strip a trailing CR (CRLF sources) */
        if (len > 0 && line[len - 1] == '\r') line[len - 1] = '\0';
        parse_line(lib, stack, &cur, line);
        if (!nl) break;
        p = nl + 1;
    }
    return lib;
}

help_lib_t *help_open_file(const char *linux_path)
{
    FILE *fp = fopen(linux_path, "r");
    if (!fp) return NULL;

    help_lib_t *lib = lib_new();
    if (!lib) { fclose(fp); return NULL; }

    help_node_t *stack[10] = {0};
    help_node_t *cur = NULL;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        parse_line(lib, stack, &cur, line);
    }
    fclose(fp);
    return lib;
}

void help_close(help_lib_t *lib)
{
    if (!lib) return;
    node_free(lib->root);
    free(lib);
}

/* ------------------------------------------------------------------ */
/* Compiled .HLB (LBRO) reading + HLP$LIBRARY search-list open          */
/* ------------------------------------------------------------------ */

/*
 * A .HLB is the OVMX "LBRO" container (dcl/hlb.h) LIBRARY/HELP/CREATE writes,
 * one module per level-1 key. Reading the modules in index order and
 * concatenating their bodies reconstructs the exact numbered-level source, so a
 * .HLB feeds the very same parser as a raw .HLP. All the helpers below return a
 * malloc'd NUL-terminated text buffer (the caller frees) so multiple libraries
 * can be concatenated for the HLP$LIBRARY search list.
 */

/* Grow-and-append `len` bytes of `add` onto *buf (*cap tracked); NUL-terminate.
 * Returns 0 on success, -1 on OOM (leaving *buf as-is for the caller to free). */
static int str_append(char **buf, size_t *len, size_t *cap,
                      const char *add, size_t add_len)
{
    if (*len + add_len + 1 > *cap) {
        size_t ncap = *cap ? *cap : 256;
        while (*len + add_len + 1 > ncap) ncap *= 2;
        char *nb = realloc(*buf, ncap);
        if (!nb) return -1;
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, add, add_len);
    *len += add_len;
    (*buf)[*len] = '\0';
    return 0;
}

/* Read a whole file into a fresh NUL-terminated buffer, or NULL. */
static char *read_whole_file(FILE *fp)
{
    if (fseek(fp, 0, SEEK_END) != 0) return NULL;
    long n = ftell(fp);
    if (n < 0) return NULL;
    if (fseek(fp, 0, SEEK_SET) != 0) return NULL;
    char *buf = malloc((size_t)n + 1);
    if (!buf) return NULL;
    if (n > 0 && (long)fread(buf, 1, (size_t)n, fp) != n) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

/* Reconstruct the numbered-level text of an already-open .HLB (its header
 * already read into *hdr, fp positioned just after the header). Concatenates
 * each module's body in index order. Returns malloc'd text, or NULL. */
static char *hlb_reconstruct_text(FILE *fp, const struct lbr_header *hdr)
{
    if (hdr->module_count > LBR_MAX_MODULES) return NULL;

    struct lbr_module *mods = NULL;
    if (hdr->module_count > 0) {
        mods = calloc(hdr->module_count, sizeof(*mods));
        if (!mods) return NULL;
        if (fread(mods, sizeof(*mods), hdr->module_count, fp)
                != hdr->module_count) {
            free(mods);
            return NULL;
        }
    }

    char *text = NULL;
    size_t len = 0, cap = 0;
    if (str_append(&text, &len, &cap, "", 0) != 0) { /* ensure non-NULL "" */
        free(mods);
        return NULL;
    }

    for (uint32_t i = 0; i < hdr->module_count; i++) {
        if (mods[i].length == 0) continue;
        char *chunk = malloc(mods[i].length);
        if (!chunk) { free(text); free(mods); return NULL; }
        if (fseek(fp, (long)mods[i].offset, SEEK_SET) != 0 ||
            fread(chunk, 1, mods[i].length, fp) != mods[i].length) {
            free(chunk); free(text); free(mods);
            return NULL;
        }
        if (str_append(&text, &len, &cap, chunk, mods[i].length) != 0) {
            free(chunk); free(text); free(mods);
            return NULL;
        }
        free(chunk);
        /* A module body ends at its last authored line; guarantee a newline
         * boundary before the next module's level-1 key. */
        if (len > 0 && text[len - 1] != '\n')
            (void)str_append(&text, &len, &cap, "\n", 1);
    }

    free(mods);
    return text;
}

/* A VMS filespec (DEV:[DIR]NAME.TYP or a device-logical spec) rather than a
 * Linux path: has a ':' or '[' and does not begin with '/'. */
static int help_is_vms_spec(const char *path)
{
    return path && path[0] != '/' &&
           (strchr(path, ':') != NULL || strchr(path, '[') != NULL);
}

/* True if the filespec's type is .HLB (a compiled binary LBRO library). The ACP
 * text reader (help_acp_library_text) reads records and would mangle a binary
 * .HLB, so a .HLB is always taken through the POSIX read path, which detects the
 * LBRO magic and reconstructs the numbered-level source. The .HLB library is a
 * build/test artifact; the product volume ships the .HLP source. */
static int help_type_is_hlb(const char *path)
{
    /* Scan the name part only (after the last ']' ':' '/'), find its last '.',
     * and compare the type -- up to a ';' version -- to "HLB". */
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == ']' || *p == ':' || *p == '/' || *p == '>')
            base = p + 1;

    const char *dot = NULL;
    for (const char *p = base; *p && *p != ';'; p++)
        if (*p == '.') dot = p;
    if (!dot) return 0;

    return (dot[1] == 'H' || dot[1] == 'h') &&
           (dot[2] == 'L' || dot[2] == 'l') &&
           (dot[3] == 'B' || dot[3] == 'b') &&
           (dot[4] == '\0' || dot[4] == ';');
}

/*
 * Return the numbered-level source text for one library, given either a VMS
 * filespec or a Linux path.
 *
 *  - A VMS spec (SYS$HELP:HELPLIB.HLP, HLP$LIBRARY translations) is read over
 *    the Files-11 ACP first (the weak help_acp_library_text seam) -- the product
 *    runtime, where /dev/vms is present and the /vms passthrough is gone. If the
 *    ACP cannot reach it (host build/test tooling with no /dev/vms, or the
 *    netbsd-vax cross), it FALLS BACK to a POSIX read of the translated /vms
 *    path (help_acp_vms_to_linux). On the real runtime a file that is genuinely
 *    absent fails BOTH the ACP read and the POSIX open (/vms does not exist
 *    there), so HELP reports %HELP-E-OPENIN honestly -- never a fabricated
 *    success (Rule 9 / INV-6). This is the same ACP-first, POSIX-for-host dual
 *    backend $SEARCH (rms_search.c) uses. When the seam is absent (engine unit
 *    test), a VMS spec has no reader and yields NULL.
 *  - A Linux path (the $OVMX_HELPLIB locator; temp-file engine tests) is read
 *    directly.
 *
 * Either way, a compiled .HLB (LBRO magic) is auto-detected on the POSIX read
 * and reconstructed to its numbered-level source; a raw .HLP is returned as-is.
 * Malloc'd, or NULL.
 */
static char *library_source_text(const char *path)
{
    if (help_is_vms_spec(path)) {
        /* A .HLP source is line-oriented, so it reads over the ACP text reader;
         * a binary .HLB does NOT (the record reader would mangle it) -- it goes
         * straight to the POSIX read below, which detects the LBRO magic and
         * reconstructs the source. */
        if (!help_type_is_hlb(path) && &help_acp_library_text) {
            char *text = help_acp_library_text(path);   /* ACP; NULL on a miss */
            if (text)
                return text;
        }
        /* POSIX fallback (host tooling / netbsd cross), and the .HLB read path:
         * translate to the /vms path and read it as a Linux path below (fopen +
         * .HLB/.HLP auto-detect). On the product runtime /vms does not exist, so
         * this open fails too -- an honest miss, not a fake (Rule 9 / INV-6). */
        if (&help_acp_vms_to_linux) {
            char linux_path[1024];
            if (help_acp_vms_to_linux(path, linux_path, sizeof(linux_path)))
                return library_source_text(linux_path);   /* now a Linux path */
        }
        return NULL;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    struct lbr_header hdr;
    char *text;
    if (fread(&hdr, sizeof(hdr), 1, fp) == 1 && hdr.magic == LBR_MAGIC &&
        hdr.type == LBR_TYPE_HELP) {
        text = hlb_reconstruct_text(fp, &hdr);
    } else {
        text = read_whole_file(fp);
    }
    fclose(fp);
    return text;
}

help_lib_t *help_open_hlb(const char *linux_path)
{
    FILE *fp = fopen(linux_path, "rb");
    if (!fp) return NULL;
    struct lbr_header hdr;
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1 || hdr.magic != LBR_MAGIC ||
        hdr.type != LBR_TYPE_HELP) {
        fclose(fp);
        return NULL;
    }
    char *text = hlb_reconstruct_text(fp, &hdr);
    fclose(fp);
    if (!text) return NULL;
    help_lib_t *lib = help_open_text(text);
    free(text);
    return lib;
}

help_lib_t *help_open_any(const char *linux_path)
{
    char *text = library_source_text(linux_path);
    if (!text) return NULL;
    help_lib_t *lib = help_open_text(text);
    free(text);
    return lib;
}

help_lib_t *help_open_libraries(const char *const paths[], int n)
{
    if (!paths || n <= 0) return NULL;

    char *combined = NULL;
    size_t len = 0, cap = 0;
    int any = 0;

    for (int i = 0; i < n; i++) {
        if (!paths[i]) continue;
        char *text = library_source_text(paths[i]);
        if (!text) continue;
        /* Keep libraries newline-separated so a key line never fuses onto the
         * previous library's trailing body line. */
        if (any && len > 0 && combined[len - 1] != '\n')
            (void)str_append(&combined, &len, &cap, "\n", 1);
        if (str_append(&combined, &len, &cap, text, strlen(text)) != 0) {
            free(text);
            free(combined);
            return NULL;
        }
        free(text);
        any = 1;
    }

    if (!any) {
        free(combined);
        return NULL;
    }

    help_lib_t *lib = help_open_text(combined);
    free(combined);
    return lib;
}

/* ------------------------------------------------------------------ */
/* Navigation                                                          */
/* ------------------------------------------------------------------ */

/* VMS HELP accepts abbreviated keys: a query matches a key if the key begins
 * with the query, case-insensitively. */
static int key_match(const char *key, const char *query)
{
    if (!query[0]) return 0;
    return strncasecmp(key, query, strlen(query)) == 0;
}

static help_node_t *child_lookup(help_node_t *parent, const char *query)
{
    /* Prefer an exact (case-insensitive) match; fall back to first prefix. */
    help_node_t *prefix = NULL;
    for (help_node_t *c = parent->first_child; c; c = c->next_sibling) {
        if (strcasecmp(c->name, query) == 0) return c;
        if (!prefix && key_match(c->name, query)) prefix = c;
    }
    return prefix;
}

/* A qualifier key ("/ALL") is a subtopic of its command in VMS's listing
 * ("Parameter  Qualifiers / /ALL /ERASE ... / Examples", keystroke HLP.NAV);
 * OVMX's library files them under a "Qualifiers" subtopic, so a "/" key not
 * found directly is looked for there (rd vms-f9e). */
static help_node_t *key_lookup(help_node_t *parent, const char *query)
{
    help_node_t *n = child_lookup(parent, query);
    if (!n && query[0] == '/') {
        help_node_t *q = child_lookup(parent, "Qualifiers");
        if (q && strcasecmp(q->name, "Qualifiers") == 0)
            n = child_lookup(q, query);
    }
    return n;
}

help_node_t *help_find(help_lib_t *lib, const char *const path[], int n)
{
    if (!lib) return NULL;
    help_node_t *node = lib->root;
    for (int i = 0; i < n; i++) {
        node = key_lookup(node, path[i]);
        if (!node) return NULL;
    }
    return (n == 0) ? NULL : node;
}

/* ------------------------------------------------------------------ */
/* Node mutation (used by callers that inject content -- e.g. the DCL   */
/* built-in HELP folding the Engine A CDU command tables into the       */
/* library tree; vms-01b). Kept in the engine so it stays free of any   */
/* DCL/libvms dependency and the hermetic unit test can exercise it.    */
/* ------------------------------------------------------------------ */

help_node_t *help_node_find_child(help_node_t *parent, const char *name)
{
    if (!parent || !name) return NULL;
    return child_lookup(parent, name);
}

help_node_t *help_node_add_child(help_node_t *parent, int level,
                                 const char *name)
{
    if (!parent || !name) return NULL;
    help_node_t *node = node_new(level, name);
    if (!node) return NULL;
    node_add_child(parent, node);
    return node;
}

void help_node_set_text(help_node_t *node, const char *text)
{
    if (!node) return;
    free(node->text);
    node->text = NULL;
    if (text && text[0]) {
        /* Stored verbatim (may contain embedded newlines); help_show_node
         * prints it as-is, so callers include their own body indentation. */
        node->text = help_strdup(text);
    }
}

void help_node_clear_children(help_node_t *node)
{
    if (!node) return;
    help_node_t *c = node->first_child;
    while (c) {
        help_node_t *next = c->next_sibling;
        node_free(c);
        c = next;
    }
    node->first_child = NULL;
    node->last_child = NULL;
}

void help_node_remove_child(help_node_t *parent, help_node_t *child)
{
    if (!parent || !child) return;
    help_node_t *prev = NULL;
    for (help_node_t *c = parent->first_child; c; c = c->next_sibling) {
        if (c == child) {
            if (prev)
                prev->next_sibling = c->next_sibling;
            else
                parent->first_child = c->next_sibling;
            if (parent->last_child == c)
                parent->last_child = prev;
            c->next_sibling = NULL;
            node_free(c);
            return;
        }
        prev = c;
    }
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

/*
 * HELP's layout, as the VAX V7.3 console shows it (keystroke HLP.NAV, probe
 * Q.HELP; rd vms-f9e):
 *
 *   - a node is a blank line, then each key of its path on its own line,
 *     indented two spaces a level and followed by a blank line, then the
 *     node's text, indented two spaces a level below the first;
 *   - its subtopics follow two blank lines later under "Additional
 *     information available:", indented two spaces a level, in 11-character
 *     columns (a longer key takes as many as it needs); a key starts a new
 *     line when it would end past column 77, and qualifier keys ("/X") sit
 *     on lines of their own;
 *   - a blank line ends the display, before the next prompt.
 */
#define HELP_COLW   11
#define HELP_LIMIT  77

static int listing_slash(const help_node_t *c) { return c->name[0] == '/'; }

/* the keys a node lists: its children, with a "Qualifiers" child's own
 * children (the qualifiers) listed right after it */
static int collect_keys(help_node_t *node, help_node_t **keys, int max)
{
    int n = 0;
    for (help_node_t *c = node ? node->first_child : NULL; c && n < max;
         c = c->next_sibling) {
        keys[n++] = c;
        if (strcasecmp(c->name, "Qualifiers") == 0)
            for (help_node_t *q = c->first_child; q && n < max; q = q->next_sibling)
                keys[n++] = q;
    }
    return n;
}

static void print_key_columns_at(help_node_t *node, int indent, FILE *out)
{
    help_node_t *keys[1024];
    int n = collect_keys(node, keys, 1024);
    int pos = -1;                        /* -1: nothing on this line yet */
    int prev_slash = -1;

    for (int i = 0; i < n; i++) {
        const char *k = keys[i]->name;
        int len = (int)strlen(k);
        int slash = listing_slash(keys[i]);
        if (pos >= 0 && (slash != prev_slash || pos + len > HELP_LIMIT)) {
            fputc('\n', out);
            pos = -1;
        }
        if (pos < 0) {
            fprintf(out, "%*s", indent, "");
            pos = indent;
        }
        fputs(k, out);
        {
            int cols = (len + 1 + HELP_COLW - 1) / HELP_COLW;
            int next = pos + cols * HELP_COLW;
            /* the padding is written only when another key follows */
            int more = (i + 1 < n);
            int nslash = more ? listing_slash(keys[i + 1]) : slash;
            int nlen = more ? (int)strlen(keys[i + 1]->name) : 0;
            if (more && nslash == slash && next + nlen <= HELP_LIMIT)
                fprintf(out, "%*s", next - pos - len, "");
            pos = next;
        }
        prev_slash = slash;
    }
    if (pos >= 0)
        fputc('\n', out);
}

static void print_key_columns(help_node_t *first, FILE *out)
{
    print_key_columns_at(first ? first->parent : NULL, 2, out);
}

/* the node's text, each non-empty line indented `indent` more */
static void print_text(const char *text, int indent, FILE *out)
{
    const char *p = text;
    while (p && *p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l)
            fprintf(out, "%*s%.*s", indent, "", (int)l, p);
        fputc('\n', out);
        p = e ? e + 1 : p + l;
    }
}

static void print_listing(help_node_t *node, int indent, FILE *out)
{
    fprintf(out, "\n\n%*sAdditional information available:\n\n", indent, "");
    print_key_columns_at(node, indent, out);
}

void help_show_toplevel(help_lib_t *lib, FILE *out)
{
    const char *self[] = { "HELP" };
    help_node_t *h = lib ? help_find(lib, self, 1) : NULL;

    /* VMS's bare HELP shows the HELP topic, then every topic (probe Q.HELP E) */
    if (h && strcasecmp(h->name, "HELP") == 0) {
        fprintf(out, "\nHELP\n\n");
        if (h->text && h->text[0])
            print_text(h->text, 0, out);
        print_listing(lib->root, 2, out);
    } else if (lib && lib->root->first_child) {
        fprintf(out, "\n  Information available:\n\n");
        print_key_columns_at(lib->root, 2, out);
    } else {
        fprintf(out, "\n  (no information available)\n");
    }
}

/* the node at each level of the path, for the header */
static void write_header(help_lib_t *lib, const char *const path[], int n,
                         FILE *out)
{
    help_node_t *node = lib ? lib->root : NULL;
    for (int i = 0; i < n; i++) {
        help_node_t *c = node ? key_lookup(node, path[i]) : NULL;
        const char *nm = c ? c->name : path[i];
        fprintf(out, "%*s", 2 * i, "");
        for (const char *p = nm; *p; p++)
            fputc(toupper((unsigned char)*p), out);
        fputs("\n\n", out);
        node = c;
    }
}

static void show_node_lib(help_lib_t *lib, help_node_t *node,
                          const char *const path[], int n, FILE *out)
{
    if (!node) return;
    fputc('\n', out);
    write_header(lib, path, n, out);
    if (node->text && node->text[0])
        print_text(node->text, n > 1 ? 2 * (n - 1) : 0, out);
    if (node->first_child)
        print_listing(node, n > 0 ? 2 * n : 2, out);
}

static help_lib_t *g_show_lib;

void help_show_node(help_node_t *node, const char *const path[], int n,
                    FILE *out)
{
    show_node_lib(g_show_lib, node, path, n, out);
}

/* "  Sorry, no documentation on <PATH>", then what the level above offers */
static void show_nodoc(const char *const path[], int n, FILE *out)
{
    fprintf(out, "  Sorry, no documentation on ");
    for (int i = 0; i < n; i++) {
        if (i) fputc(' ', out);
        for (const char *p = path[i]; *p; p++)
            fputc(toupper((unsigned char)*p), out);
    }
    fputc('\n', out);
}

int help_render(help_lib_t *lib, const char *const path[], int n, FILE *out)
{
    g_show_lib = lib;
    if (n == 0) {
        help_show_toplevel(lib, out);
        return SS$_NORMAL;
    }
    help_node_t *node = help_find(lib, path, n);
    if (!node) {
        show_nodoc(path, n, out);
        return SS$_ITEMNOTFOUND;
    }
    show_node_lib(lib, node, path, n, out);
    return SS$_NORMAL;
}

/* ------------------------------------------------------------------ */
/* Interactive prompt loop                                             */
/* ------------------------------------------------------------------ */

#define HELP_MAX_DEPTH 9

static help_read_fn g_reader;

void help_set_reader(help_read_fn fn)
{
    g_reader = fn;
}

/* "Topic? " at the top, "<KEY> ... Subtopic? " inside a topic: a prompt on
 * a new line of its own (no fill), as INQUIRE's is */
static int read_prompted(char stack[][128], int depth, char *line,
                         size_t sz, FILE *in, FILE *out)
{
    char prompt[HELP_MAX_DEPTH * 130 + 16];
    size_t pl = 0;

    prompt[pl++] = '\r';
    prompt[pl++] = '\n';
    for (int i = 0; i < depth; i++) {
        for (const char *p = stack[i]; *p && pl < sizeof prompt - 16; p++)
            prompt[pl++] = (char)toupper((unsigned char)*p);
        prompt[pl++] = ' ';
    }
    memcpy(prompt + pl, depth ? "Subtopic? " : "Topic? ", depth ? 11 : 8);
    if (g_reader) {
        fflush(out);
        return g_reader(prompt, line, sz);
    }
    fputs(prompt + 1, out);       /* a plain stream: "\nTopic? " */
    fflush(out);
    if (!fgets(line, (int)sz, in))
        return -1;
    return 0;
}

static void show_level(help_lib_t *lib, char stack[][128], int depth,
                       FILE *out)
{
    if (depth == 0) {
        help_show_toplevel(lib, out);
    } else {
        const char *path[HELP_MAX_DEPTH];
        for (int i = 0; i < depth; i++) path[i] = stack[i];
        show_node_lib(lib, help_find(lib, path, depth), path, depth, out);
    }
    fputc('\n', out);             /* the blank line that ends a display */
}

/* the display after "Sorry": what the level the user is at offers */
static void show_offer(help_lib_t *lib, char stack[][128], int depth, FILE *out)
{
    help_node_t *node = lib->root;
    if (depth) {
        const char *path[HELP_MAX_DEPTH];
        for (int i = 0; i < depth; i++) path[i] = stack[i];
        node = help_find(lib, path, depth);
    }
    if (node && node->first_child)
        print_listing(node, depth ? 2 * depth : 2, out);
    fputc('\n', out);
}

void help_interactive(help_lib_t *lib, const char *const initial[], int ninit,
                      FILE *in, FILE *out)
{
    char stack[HELP_MAX_DEPTH][128];
    int depth = 0;

    g_show_lib = lib;
    if (ninit > 0) {
        const char *path[HELP_MAX_DEPTH];
        int want = ninit < HELP_MAX_DEPTH ? ninit : HELP_MAX_DEPTH;
        for (int i = 0; i < want; i++) path[i] = initial[i];
        help_node_t *node = help_find(lib, path, want);
        if (node) {
            for (int i = 0; i < want; i++) {
                snprintf(stack[i], sizeof stack[0], "%s", initial[i]);
            }
            show_level(lib, stack, want, out);
            /* a topic named on the command line with nothing below it
             * returns to "Topic? " (probe Q.HELP A) */
            depth = node->first_child ? want : 0;
        } else {
            show_nodoc(path, want, out);
            show_offer(lib, stack, 0, out);
        }
    } else {
        show_level(lib, stack, 0, out);
    }

    char line[256];
    for (;;) {
        if (read_prompted(stack, depth, line, sizeof line, in, out) != 0)
            break;                         /* CTRL/Z or EOF */

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                           line[len - 1] == ' ' || line[len - 1] == '\t'))
            line[--len] = '\0';
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;

        if (*s == '\0') {
            /* RETURN: up one level, no redisplay; out at the top */
            if (depth == 0) break;
            depth--;
            continue;
        }
        if (strcmp(s, "?") == 0) {
            show_level(lib, stack, depth, out);
            continue;
        }

        int base = depth;
        char *tok = strtok(s, " \t");
        while (tok && depth < HELP_MAX_DEPTH) {
            snprintf(stack[depth], sizeof stack[0], "%s", tok);
            depth++;
            const char *path[HELP_MAX_DEPTH];
            for (int i = 0; i < depth; i++) path[i] = stack[i];
            help_node_t *node = help_find(lib, path, depth);
            if (!node) {
                show_nodoc(path, depth, out);
                depth = base;
                show_offer(lib, stack, depth, out);
                break;
            }
            tok = strtok(NULL, " \t");
            if (!tok) {
                show_level(lib, stack, depth, out);
                /* a key with nothing below it: stay where the user was */
                if (!node->first_child)
                    depth = base;
            }
        }
    }
}
