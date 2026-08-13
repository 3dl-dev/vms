/*
 * dcl/help.h - Hierarchical HELP library engine (vms-01b)
 *
 * A faithful reader for OpenVMS HELP libraries. VMS HELP text is authored in a
 * numbered-level source format (the ".HLP" input LIBRARIAN compiles into a
 * ".HLB"): each key line begins with a level digit (1..9) followed by the key
 * name, and the lines beneath it are that key's body text. Level 1 keys are
 * top-level topics, level 2 their subtopics, and so on -- the hierarchy VMS
 * HELP navigates with "HELP topic subtopic ...", the "Additional information
 * available:" subtopic listing, and the "Topic?" / "<path> Subtopic?" prompt
 * loop.
 *
 * Clean-room provenance (project Rule 8): the numbered-level HELP source
 * format, the "Information available:" / "Additional information available:"
 * listing headers, the "Topic?" / "Subtopic?" prompt wording, and the
 * "Sorry, no documentation on <topic>" not-found message are all taken from
 * public OpenVMS documentation and observed HELP output -- the VSI OpenVMS
 * DCL Dictionary (HELP) and the VSI OpenVMS User's Manual ("Getting Help").
 * The reader consumes both the documented numbered-level ".HLP" source form
 * (help_open_file) and the compiled ".HLB" help library (help_open_hlb): a
 * ".HLB" is the OVMX "LBRO" container (dcl/hlb.h) LIBRARY/HELP/CREATE writes,
 * with one module per level-1 key. Its byte layout is an OVMX design choice
 * (Rule 8) -- the VMS ".HLB" byte layout is unpublished -- and reconstructs the
 * exact numbered-level text, so once loaded a ".HLB" is indistinguishable from
 * its ".HLP" source. HELP locates its library through the HLP$LIBRARY,
 * HLP$LIBRARY_1..n search list (help_open_libraries), the documented VMS HELP
 * library search order (VSI OpenVMS DCL Dictionary, HELP).
 *
 * This engine is deliberately free of DCL-context and libvms-runtime
 * dependencies (only <stdio.h> for the FILE* sinks and VMS status codes) so it
 * can be exercised standalone by a hermetic unit test.
 */

#ifndef OVMX_DCL_HELP_H
#define OVMX_DCL_HELP_H

#include <stdio.h>

/* One key in the hierarchy (topic, subtopic, ...). */
typedef struct help_node {
    int                level;         /* 1 = top-level topic, 2 = subtopic ... */
    char              *name;          /* key name (as authored)                */
    char              *text;          /* body text (may be NULL/empty)         */
    struct help_node  *parent;
    struct help_node  *first_child;
    struct help_node  *last_child;
    struct help_node  *next_sibling;
} help_node_t;

/* A parsed help library. root is a synthetic level-0 node whose children are
 * the level-1 topics, in authored order. */
typedef struct help_lib {
    help_node_t *root;
} help_lib_t;

/* Parse a HELP library from an in-memory source string. Returns NULL on OOM. */
help_lib_t *help_open_text(const char *text);

/* Parse a HELP library from a file (numbered-level .HLP source). Returns NULL
 * if the file cannot be opened or on OOM. */
help_lib_t *help_open_file(const char *linux_path);

/* Parse a HELP library from a compiled .HLB (the OVMX "LBRO" container written
 * by LIBRARY/HELP/CREATE; dcl/hlb.h). Each module is a level-1 key's subtree;
 * the modules are read in index order and reconstruct the numbered-level tree.
 * Returns NULL if the file is not a readable HELP library or on OOM. */
help_lib_t *help_open_hlb(const char *linux_path);

/* Open one library file, auto-detecting .HLB (LBRO magic) vs .HLP source. */
help_lib_t *help_open_any(const char *linux_path);

/* Open an ordered HLP$LIBRARY search list and merge it into one library: the
 * paths (each .HLB or .HLP) are loaded in order, so a key defined in an earlier
 * library takes precedence (VMS HELP searches HLP$LIBRARY, HLP$LIBRARY_1..n in
 * order and the first match wins), and the top level is the union of all keys.
 * Returns NULL if none could be opened or on OOM. */
help_lib_t *help_open_libraries(const char *const paths[], int n);

/* Release a library and all its nodes. */
void help_close(help_lib_t *lib);

/* Resolve a topic path (case-insensitive prefix match at each level, exactly
 * as VMS HELP accepts abbreviated keys). Returns the matched node, or NULL if
 * any component cannot be resolved. path may be NULL when n == 0. */
help_node_t *help_find(help_lib_t *lib, const char *const path[], int n);

/* ---- Node mutation (content injection) ---------------------------------- */
/* These let a caller graft content onto an open library tree without any
 * DCL/libvms dependency in the engine -- the seam the DCL built-in HELP uses
 * to fold the Engine A CDU command tables in (per-command qualifiers) so the
 * help stays in sync with the actually-accepted syntax (vms-01b). */

/* Return the immediate child of `parent` matching `name` (exact match preferred,
 * else the first case-insensitive prefix match, exactly as topic lookup), or
 * NULL. */
help_node_t *help_node_find_child(help_node_t *parent, const char *name);

/* Append a new child key `name` at `level` under `parent`; returns it, or NULL
 * on bad args / OOM. */
help_node_t *help_node_add_child(help_node_t *parent, int level,
                                 const char *name);

/* Replace a node's body text (NULL/empty clears it). Stored verbatim, so the
 * caller supplies its own line layout (leading space, trailing newlines). */
void help_node_set_text(help_node_t *node, const char *text);

/* Free and detach all children of `node` (the node itself is kept). */
void help_node_clear_children(help_node_t *node);

/* Detach `child` from `parent`'s child list and free its subtree. */
void help_node_remove_child(help_node_t *parent, help_node_t *child);

/* Print the top-level "Information available:" topic listing. */
void help_show_toplevel(help_lib_t *lib, FILE *out);

/* Print one node: its "<PATH>" header, indented body text, and the
 * "Additional information available:" listing of its immediate subtopics. */
void help_show_node(help_node_t *node, const char *const path[], int n,
                    FILE *out);

/* Non-interactive lookup + display. n == 0 lists the top level; otherwise the
 * resolved node is shown, or "Sorry, no documentation on <PATH>" is printed
 * when it cannot be resolved. Returns a VMS status code:
 *   SS$_NORMAL       - a node (or the top-level list) was displayed
 *   SS$_ITEMNOTFOUND - the requested topic path had no documentation */
int help_render(help_lib_t *lib, const char *const path[], int n, FILE *out);

/* Interactive HELP: display the initial path (top level when ninit == 0), then
 * drive the VMS "Topic?" / "<path> Subtopic?" prompt loop reading from `in`.
 * A blank line pops one level (and exits at the top); EOF exits. */
void help_interactive(help_lib_t *lib, const char *const initial[], int ninit,
                      FILE *in, FILE *out);

#endif /* OVMX_DCL_HELP_H */
