/*
 * vms_help.c - VMS HELP utility for OVMX
 *
 * Provides an interactive, hierarchical help system similar to the
 * OpenVMS HELP command.  Help text is stored in structured help
 * files with numbered topic levels:
 *
 *   1 DIRECTORY
 *   Displays a list of files.
 *   2 Qualifiers
 *   3 /BRIEF
 *   Displays only file names.
 *
 * The utility can be invoked as:
 *   HELP                    - show top-level topics
 *   HELP topic              - show help for a topic
 *   HELP topic subtopic     - show help for a subtopic
 *
 * Build: gcc -o vms_help vms_help.c -lvms
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Maximum nesting depth for help topics */
#define MAX_DEPTH  9

/* Maximum line length in help file */
#define MAX_LINE   1024

/* Default help library path */
#include "ovmx_layout.h"
#include "vmsfs/device.h"
#include "vmsfs/filespec.h"
#include "vms/logical.h"
#define DEFAULT_HELP_PATH VMS_HELPLIB_PATH

/* ------------------------------------------------------------------ */
/* Built-in help text (used if no help file is found)                  */
/* ------------------------------------------------------------------ */
static const char *builtin_help =
    "1 COPY\n"
    " Copies a file.\n"
    " Format: COPY source destination\n"
    "2 Qualifiers\n"
    "3 /CONFIRM\n"
    " Prompts before each copy operation.\n"
    "3 /LOG\n"
    " Displays each file as it is copied.\n"
    "1 DELETE\n"
    " Deletes a file.\n"
    " Format: DELETE filespec[,...]\n"
    "2 Qualifiers\n"
    "3 /CONFIRM\n"
    " Prompts before each delete.\n"
    "3 /LOG\n"
    " Displays each file as it is deleted.\n"
    "1 DIRECTORY\n"
    " Lists files in a directory.\n"
    " Format: DIRECTORY [filespec]\n"
    "2 Qualifiers\n"
    "3 /BRIEF\n"
    " Displays only file names.\n"
    "3 /FULL\n"
    " Displays full file information.\n"
    "3 /SIZE\n"
    " Displays file sizes.\n"
    "3 /DATE\n"
    " Displays file dates.\n"
    "3 /OWNER\n"
    " Displays file owners.\n"
    "1 EXIT\n"
    " Exits the current command level.\n"
    " Format: EXIT [status-code]\n"
    "1 HELP\n"
    " Displays help information.\n"
    " Format: HELP [topic [subtopic [...]]]\n"
    " You can type topic names at the \"Topic?\" prompt.\n"
    " Press RETURN for a list of topics at the current level.\n"
    " Type a period (.) to go up one level.\n"
    "1 LOGOUT\n"
    " Ends the current session.\n"
    " Format: LOGOUT\n"
    "1 SET\n"
    " Modifies process characteristics.\n"
    "2 DEFAULT\n"
    " Sets the default directory.\n"
    " Format: SET DEFAULT directory-spec\n"
    "2 PROMPT\n"
    " Changes the DCL prompt.\n"
    " Format: SET PROMPT string\n"
    "2 VERIFY\n"
    " Enables/disables command procedure verification.\n"
    " Format: SET [NO]VERIFY\n"
    "1 SHOW\n"
    " Displays system and process information.\n"
    "2 DEFAULT\n"
    " Displays the current default directory.\n"
    " Format: SHOW DEFAULT\n"
    "2 LOGICAL\n"
    " Displays logical name definitions.\n"
    " Format: SHOW LOGICAL [name]\n"
    "2 PROCESS\n"
    " Displays process information.\n"
    " Format: SHOW PROCESS\n"
    "2 SYMBOL\n"
    " Displays symbol values.\n"
    " Format: SHOW SYMBOL symbol-name\n"
    "2 SYSTEM\n"
    " Displays all processes.\n"
    " Format: SHOW SYSTEM\n"
    "2 TIME\n"
    " Displays current date and time.\n"
    " Format: SHOW TIME\n"
    "1 TYPE\n"
    " Displays the contents of a file.\n"
    " Format: TYPE filespec\n"
    "2 Qualifiers\n"
    "3 /PAGE\n"
    " Pauses display at each screen.\n"
    "";

/* ------------------------------------------------------------------ */
/* Help entry (parsed from file or built-in)                          */
/* ------------------------------------------------------------------ */
typedef struct help_entry {
    int                  level;
    char                 name[128];
    char                *text;           /* Help text (malloc'd) */
    size_t               text_len;
    size_t               text_cap;
    struct help_entry   *next;           /* Next entry at any level */
} help_entry_t;

static help_entry_t *help_list = NULL;

/* ------------------------------------------------------------------ */
/* Free the entire help list                                          */
/* ------------------------------------------------------------------ */
static void free_help_list(void)
{
    help_entry_t *e = help_list;
    while (e) {
        help_entry_t *next = e->next;
        free(e->text);
        free(e);
        e = next;
    }
    help_list = NULL;
}

/* ------------------------------------------------------------------ */
/* Append text to a help entry                                        */
/* ------------------------------------------------------------------ */
static void entry_append(help_entry_t *e, const char *line)
{
    size_t len = strlen(line);
    if (e->text_len + len + 2 > e->text_cap) {
        e->text_cap = (e->text_cap == 0) ? 512 : e->text_cap * 2;
        if (e->text_cap < e->text_len + len + 2)
            e->text_cap = e->text_len + len + 2;
        e->text = (char *)realloc(e->text, e->text_cap);
    }
    memcpy(e->text + e->text_len, line, len);
    e->text_len += len;
    e->text[e->text_len++] = '\n';
    e->text[e->text_len] = '\0';
}

/* ------------------------------------------------------------------ */
/* Parse help text (from string or FILE*)                             */
/* ------------------------------------------------------------------ */

/* Parse a line-at-a-time.  'source' is either a FILE* or a char** */
typedef struct {
    FILE       *fp;
    const char *str_ptr;
} help_source_t;

static char *help_getline(help_source_t *src, char *buf, int bufsiz)
{
    if (src->fp) {
        return fgets(buf, bufsiz, src->fp);
    }
    /* Read from string */
    if (!src->str_ptr || *src->str_ptr == '\0')
        return NULL;
    const char *end = strchr(src->str_ptr, '\n');
    size_t len;
    if (end) {
        len = (size_t)(end - src->str_ptr);
        if (len >= (size_t)(bufsiz - 1))
            len = (size_t)(bufsiz - 1);
        memcpy(buf, src->str_ptr, len);
        buf[len] = '\0';
        src->str_ptr = end + 1;
    } else {
        len = strlen(src->str_ptr);
        if (len >= (size_t)(bufsiz - 1))
            len = (size_t)(bufsiz - 1);
        memcpy(buf, src->str_ptr, len);
        buf[len] = '\0';
        src->str_ptr += len;
    }
    return buf;
}

static void parse_help(help_source_t *src)
{
    char line[MAX_LINE];
    help_entry_t *tail = NULL;
    help_entry_t *current = NULL;

    while (help_getline(src, line, sizeof(line))) {
        /* Trim trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        /* Check if this is a topic header line (e.g. "1 SHOW") */
        if (line[0] >= '1' && line[0] <= '9' && line[1] == ' ') {
            int level = line[0] - '0';
            const char *name = line + 2;

            /* Skip leading whitespace in name */
            while (*name == ' ') name++;

            help_entry_t *entry = (help_entry_t *)calloc(1, sizeof(help_entry_t));
            entry->level = level;
            strncpy(entry->name, name, sizeof(entry->name) - 1);

            /* Link into list */
            if (!help_list) {
                help_list = entry;
            } else {
                tail->next = entry;
            }
            tail = entry;
            current = entry;
        } else if (current) {
            /* Body text for the current entry */
            entry_append(current, line);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Load help from file or built-in                                    */
/* ------------------------------------------------------------------ */
static void load_help(const char *path)
{
    free_help_list();

    help_source_t src = {0};
    FILE *fp = NULL;

    if (path)
        fp = fopen(path, "r");

    if (fp) {
        src.fp = fp;
        parse_help(&src);
        fclose(fp);
    } else {
        /* Fall back to built-in help */
        src.str_ptr = builtin_help;
        parse_help(&src);
    }
}

/* ------------------------------------------------------------------ */
/* Case-insensitive prefix match                                      */
/* ------------------------------------------------------------------ */
static int name_match(const char *entry_name, const char *query)
{
    size_t qlen = strlen(query);
    if (qlen == 0)
        return 0;
    /* VMS HELP allows abbreviated topic names */
    return (strncasecmp(entry_name, query, qlen) == 0);
}

/* ------------------------------------------------------------------ */
/* Display topics at a given level with specified parent chain         */
/* parent_names[0..depth-1] are the parent topic names.               */
/* ------------------------------------------------------------------ */

/*
 * Find the starting entry for a given parent chain.
 * Returns the first child entry, or NULL.
 * Also sets *parent_entry to the matching parent (if depth > 0).
 */
static help_entry_t *find_context(const char *parents[], int depth,
                                  help_entry_t **parent_entry_out)
{
    help_entry_t *e;
    int matched_depth = 0;
    help_entry_t *parent_entry = NULL;

    if (depth == 0) {
        /* Top-level: return first level-1 entry */
        if (parent_entry_out) *parent_entry_out = NULL;
        return help_list;
    }

    /* Walk the list, tracking our position in the hierarchy */
    int context_level[MAX_DEPTH];
    help_entry_t *context_entry[MAX_DEPTH];
    memset(context_level, 0, sizeof(context_level));
    memset(context_entry, 0, sizeof(context_entry));

    for (e = help_list; e; e = e->next) {
        if (e->level <= matched_depth + 1) {
            /* This entry is at or above our current search level */
            if (e->level == matched_depth + 1 &&
                name_match(e->name, parents[matched_depth])) {
                context_entry[matched_depth] = e;
                matched_depth++;
                if (matched_depth == depth) {
                    parent_entry = e;
                    /* Now find the first child */
                    help_entry_t *child = e->next;
                    while (child && child->level > depth + 1)
                        child = child->next;
                    if (child && child->level == depth + 1) {
                        if (parent_entry_out) *parent_entry_out = parent_entry;
                        return child;
                    }
                    /* No children, but the entry exists */
                    if (parent_entry_out) *parent_entry_out = parent_entry;
                    return NULL;
                }
            } else if (e->level <= matched_depth) {
                /* Went back up; reset */
                if (e->level == 1)
                    matched_depth = 0;
                else
                    matched_depth = e->level - 1;

                /* Check if this matches at the new level */
                if (e->level == matched_depth + 1 &&
                    name_match(e->name, parents[matched_depth])) {
                    context_entry[matched_depth] = e;
                    matched_depth++;
                    if (matched_depth == depth) {
                        parent_entry = e;
                        if (parent_entry_out) *parent_entry_out = parent_entry;
                        help_entry_t *child = e->next;
                        if (child && child->level == depth + 1)
                            return child;
                        return NULL;
                    }
                }
            }
        }
    }

    if (parent_entry_out) *parent_entry_out = parent_entry;
    return NULL;
}

/*
 * Print the text of an entry and list its subtopics.
 */
static void display_entry(help_entry_t *entry)
{
    if (!entry) return;

    /* Print the entry text */
    if (entry->text && entry->text_len > 0)
        printf("\n%s\n", entry->text);
    else
        printf("\n");

    /* List subtopics at entry->level + 1 */
    int sublevel = entry->level + 1;
    help_entry_t *e;
    int count = 0;
    int past_entry = 0;

    for (e = help_list; e; e = e->next) {
        if (e == entry) {
            past_entry = 1;
            continue;
        }
        if (!past_entry) continue;

        /* Stop if we encounter another entry at the same or higher level */
        if (e->level <= entry->level)
            break;

        if (e->level == sublevel) {
            if (count == 0)
                printf("  Additional information available:\n\n");
            printf("  %-20s", e->name);
            count++;
            if (count % 3 == 0) printf("\n");
        }
    }
    if (count > 0 && count % 3 != 0)
        printf("\n");
}

/*
 * List all topics at a given level.
 */
static void list_topics(int level)
{
    help_entry_t *e;
    int count = 0;
    int parent_level = level - 1;
    (void)parent_level;

    if (level == 1) {
        printf("\nInformation available:\n\n");
        for (e = help_list; e; e = e->next) {
            if (e->level == 1) {
                printf("  %-20s", e->name);
                count++;
                if (count % 3 == 0) printf("\n");
            }
        }
    }

    if (count > 0 && count % 3 != 0)
        printf("\n");
    if (count == 0)
        printf("  No help available.\n");
}

/* ------------------------------------------------------------------ */
/* Interactive help loop                                              */
/* ------------------------------------------------------------------ */
static void interactive_help(void)
{
    char input[256];
    const char *stack[MAX_DEPTH];
    int depth = 0;

    /* Show top-level topics */
    list_topics(1);

    for (;;) {
        printf("\nTopic? ");
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL)
            break;

        /* Trim */
        size_t len = strlen(input);
        while (len > 0 && (input[len - 1] == '\n' || input[len - 1] == '\r'))
            input[--len] = '\0';

        /* Empty input = list current level, or exit if at top */
        if (len == 0) {
            if (depth == 0) break;
            depth = 0;
            list_topics(1);
            continue;
        }

        /* "." = go up one level */
        if (strcmp(input, ".") == 0) {
            if (depth > 0) depth--;
            if (depth == 0) {
                list_topics(1);
            } else {
                help_entry_t *parent = NULL;
                find_context(stack, depth, &parent);
                if (parent)
                    display_entry(parent);
            }
            continue;
        }

        /* Search for the topic */
        if (depth >= MAX_DEPTH) {
            printf("\n  Topic nesting too deep (max %d levels)\n", MAX_DEPTH);
            continue;
        }
        stack[depth] = strdup(input);
        depth++;

        help_entry_t *parent = NULL;
        find_context(stack, depth, &parent);
        if (parent) {
            display_entry(parent);
        } else {
            printf("\n  Sorry, no documentation on %s\n", input);
            free((void *)stack[depth - 1]);
            depth--;
        }
    }

    /* Free strdup'd names */
    for (int i = 0; i < depth; i++)
        free((void *)stack[i]);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    /* Bootstrap VMS namespace */
    vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT);
    lnm_setup_defaults(lnm_get_manager(), SYSDISK_MOUNT);

    static char help_linux[1024];
    vmsfs_to_linux_path(DEFAULT_HELP_PATH, help_linux, sizeof(help_linux));
    const char *help_path = help_linux;
    const char *env_path = getenv("SYS$HELP");
    if (env_path) {
        static char env_linux[512];
        static char full_path[512];
        /* If env_path looks like a VMS filespec, translate it */
        if (strchr(env_path, ':') || strchr(env_path, '[')) {
            vmsfs_to_linux_path(env_path, env_linux, sizeof(env_linux));
            snprintf(full_path, sizeof(full_path), "%s/HELPLIB.HLP", env_linux);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/HELPLIB.HLP", env_path);
        }
        help_path = full_path;
    }

    load_help(help_path);

    if (argc > 1) {
        /* Non-interactive: HELP topic [subtopic ...] */
        const char *topics[MAX_DEPTH];
        int ntopics = 0;
        for (int i = 1; i < argc && ntopics < MAX_DEPTH; i++)
            topics[ntopics++] = argv[i];

        help_entry_t *parent = NULL;
        find_context(topics, ntopics, &parent);
        if (parent) {
            display_entry(parent);
        } else {
            printf("  Sorry, no documentation on ");
            for (int i = 0; i < ntopics; i++)
                printf("%s%s", topics[i], (i < ntopics - 1) ? " " : "");
            printf("\n");
        }
    } else {
        /* Interactive mode */
        interactive_help();
    }

    free_help_list();
    return 0;
}
