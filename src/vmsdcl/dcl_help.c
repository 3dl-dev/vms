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

/* Return the numbered-level source text for one library file, auto-detecting a
 * compiled .HLB (LBRO magic) versus a raw .HLP source. Malloc'd, or NULL. */
static char *library_source_text(const char *linux_path)
{
    FILE *fp = fopen(linux_path, "rb");
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

help_node_t *help_find(help_lib_t *lib, const char *const path[], int n)
{
    if (!lib) return NULL;
    help_node_t *node = lib->root;
    for (int i = 0; i < n; i++) {
        node = child_lookup(node, path[i]);
        if (!node) return NULL;
    }
    return (n == 0) ? NULL : node;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

/* Print a set of key names in VMS-style columns. */
static void print_key_columns(help_node_t *first, FILE *out)
{
    int col = 0;
    for (help_node_t *c = first; c; c = c->next_sibling) {
        fprintf(out, "  %-18s", c->name);
        if (++col == 3) { fputc('\n', out); col = 0; }
    }
    if (col != 0) fputc('\n', out);
}

void help_show_toplevel(help_lib_t *lib, FILE *out)
{
    fprintf(out, "\n  Information available:\n\n");
    if (lib && lib->root->first_child)
        print_key_columns(lib->root->first_child, out);
    else
        fprintf(out, "  (no information available)\n");
}

/* Build "COPY /LOG" style uppercased path header. */
static void write_path_header(const char *const path[], int n, FILE *out)
{
    for (int i = 0; i < n; i++) {
        if (i) fputc(' ', out);
        for (const char *p = path[i]; *p; p++)
            fputc(toupper((unsigned char)*p), out);
    }
}

void help_show_node(help_node_t *node, const char *const path[], int n,
                    FILE *out)
{
    if (!node) return;

    fputc('\n', out);
    /* Header: the key path as typed, uppercased (VMS echoes the topic path). */
    if (path && n > 0) {
        write_path_header(path, n, out);
        fputc('\n', out);
    } else {
        fprintf(out, "%s\n", node->name);
    }

    if (node->text && node->text[0])
        fprintf(out, "%s", node->text);

    if (node->first_child) {
        fprintf(out, "\n  Additional information available:\n\n");
        print_key_columns(node->first_child, out);
    }
}

/* Print "Sorry, no documentation on <PATH>". */
static void show_nodoc(const char *const path[], int n, FILE *out)
{
    fprintf(out, "\nSorry, no documentation on ");
    write_path_header(path, n, out);
    fputc('\n', out);
}

int help_render(help_lib_t *lib, const char *const path[], int n, FILE *out)
{
    if (n == 0) {
        help_show_toplevel(lib, out);
        return SS$_NORMAL;
    }
    help_node_t *node = help_find(lib, path, n);
    if (!node) {
        show_nodoc(path, n, out);
        return SS$_ITEMNOTFOUND;
    }
    help_show_node(node, path, n, out);
    return SS$_NORMAL;
}

/* ------------------------------------------------------------------ */
/* Interactive prompt loop                                             */
/* ------------------------------------------------------------------ */

#define HELP_MAX_DEPTH 9

/* Print the prompt for the current level: "Topic? " at the top, or
 * "<KEY1> <KEY2> ... Subtopic? " within a topic (VMS wording). */
static void print_prompt(char stack[][128], int depth, FILE *out)
{
    fputc('\n', out);
    if (depth == 0) {
        fprintf(out, "Topic? ");
    } else {
        for (int i = 0; i < depth; i++) {
            for (const char *p = stack[i]; *p; p++)
                fputc(toupper((unsigned char)*p), out);
            fputc(' ', out);
        }
        fprintf(out, "Subtopic? ");
    }
    fflush(out);
}

/* Display the node (or top level) for the current stack depth. */
static void show_current(help_lib_t *lib, char stack[][128], int depth,
                         FILE *out)
{
    if (depth == 0) {
        help_show_toplevel(lib, out);
        return;
    }
    const char *path[HELP_MAX_DEPTH];
    for (int i = 0; i < depth; i++) path[i] = stack[i];
    help_node_t *node = help_find(lib, path, depth);
    if (node)
        help_show_node(node, path, depth, out);
}

void help_interactive(help_lib_t *lib, const char *const initial[], int ninit,
                      FILE *in, FILE *out)
{
    char stack[HELP_MAX_DEPTH][128];
    int depth = 0;

    /* Seed the stack from the initial path, descending as far as it resolves.
     * An initial component that does not resolve prints the not-found message
     * and leaves us at the last good level (matching VMS). */
    if (ninit > 0) {
        const char *path[HELP_MAX_DEPTH];
        int want = ninit < HELP_MAX_DEPTH ? ninit : HELP_MAX_DEPTH;
        for (int i = 0; i < want; i++) path[i] = initial[i];
        help_node_t *node = help_find(lib, path, want);
        if (node) {
            for (int i = 0; i < want; i++) {
                strncpy(stack[i], initial[i], sizeof(stack[0]) - 1);
                stack[i][sizeof(stack[0]) - 1] = '\0';
            }
            depth = want;
            show_current(lib, stack, depth, out);
        } else {
            show_nodoc(path, want, out);
            depth = 0;
            show_current(lib, stack, depth, out);
        }
    } else {
        show_current(lib, stack, depth, out);
    }

    char line[256];
    for (;;) {
        print_prompt(stack, depth, out);

        if (!fgets(line, sizeof(line), in))
            break; /* EOF */

        /* Trim trailing newline/CR and surrounding whitespace. */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                           line[len - 1] == ' ' || line[len - 1] == '\t'))
            line[--len] = '\0';
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;

        if (*s == '\0') {
            /* Blank line: pop one level; exit at the top. */
            if (depth == 0) break;
            depth--;
            show_current(lib, stack, depth, out);
            continue;
        }

        /* Descend through each whitespace-separated key on the line. */
        char *tok = strtok(s, " \t");
        while (tok) {
            if (depth >= HELP_MAX_DEPTH) break;
            strncpy(stack[depth], tok, sizeof(stack[0]) - 1);
            stack[depth][sizeof(stack[0]) - 1] = '\0';
            depth++;

            const char *path[HELP_MAX_DEPTH];
            for (int i = 0; i < depth; i++) path[i] = stack[i];
            help_node_t *node = help_find(lib, path, depth);
            if (!node) {
                show_nodoc(path, depth, out);
                depth--; /* stay at last good level */
                break;
            }
            tok = strtok(NULL, " \t");
            if (!tok) show_current(lib, stack, depth, out);
        }
    }
}
