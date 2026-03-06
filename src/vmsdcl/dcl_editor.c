/*
 * dcl_editor.c - EDT-compatible Line Editor
 *
 * Implements a minimal EDT line-mode editor for the DCL EDIT command.
 * Supports INSERT, TYPE, DELETE, SUBSTITUTE, WRITE, EXIT, QUIT, HELP,
 * and line number navigation.
 *
 * EDT prompt is '*'. Insert mode ends on Ctrl+Z (EOF) or empty line.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "ssdef.h"

/* Maximum lines and line length */
#define EDT_MAX_LINES   65536
#define EDT_MAX_LINE    1024

/* Editor buffer */
struct edt_buffer {
    char **lines;       /* Array of line pointers */
    int    count;       /* Number of lines */
    int    capacity;    /* Allocated capacity */
    int    dot;         /* Current line (1-based, 0 = before first line) */
    char   filename[1024];
    int    modified;    /* Buffer has unsaved changes */
};

static int edt_init(struct edt_buffer *buf)
{
    buf->capacity = 256;
    buf->lines = calloc(buf->capacity, sizeof(char *));
    if (!buf->lines) return 0;
    buf->count = 0;
    buf->dot = 0;
    buf->modified = 0;
    buf->filename[0] = '\0';
    return 1;
}

static void edt_free(struct edt_buffer *buf)
{
    for (int i = 0; i < buf->count; i++)
        free(buf->lines[i]);
    free(buf->lines);
    buf->lines = NULL;
    buf->count = 0;
    buf->capacity = 0;
}

static int edt_grow(struct edt_buffer *buf)
{
    if (buf->count < buf->capacity) return 1;
    int new_cap = buf->capacity * 2;
    if (new_cap > EDT_MAX_LINES) new_cap = EDT_MAX_LINES;
    if (new_cap <= buf->capacity) return 0;
    char **new_lines = realloc(buf->lines, new_cap * sizeof(char *));
    if (!new_lines) return 0;
    buf->lines = new_lines;
    buf->capacity = new_cap;
    return 1;
}

/* Insert a line after position pos (0 = before first line) */
static int edt_insert_line(struct edt_buffer *buf, int pos, const char *text)
{
    if (!edt_grow(buf)) return 0;
    if (pos < 0) pos = 0;
    if (pos > buf->count) pos = buf->count;

    /* Shift lines down */
    for (int i = buf->count; i > pos; i--)
        buf->lines[i] = buf->lines[i - 1];

    buf->lines[pos] = strdup(text);
    if (!buf->lines[pos]) return 0;
    buf->count++;
    buf->modified = 1;
    return 1;
}

/* Load file into buffer */
static int edt_load(struct edt_buffer *buf, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    char line[EDT_MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        if (!edt_insert_line(buf, buf->count, line)) {
            fclose(fp);
            return 0;
        }
    }
    fclose(fp);
    buf->modified = 0;
    if (buf->count > 0) buf->dot = 1;
    return 1;
}

/* Save buffer to file */
static int edt_save(struct edt_buffer *buf, const char *path)
{
    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "%%EDT-E-WRITEERR, error writing %s - %s\n",
                path, strerror(errno));
        return 0;
    }
    for (int i = 0; i < buf->count; i++)
        fprintf(fp, "%s\n", buf->lines[i]);
    fclose(fp);
    buf->modified = 0;
    return 1;
}

/* Parse a range string. Supports:
 *   ""      -> current line
 *   "n"     -> line n
 *   "n:m"   -> lines n through m
 *   "."     -> current line
 *   "*"     -> all lines
 *   "REST"  -> current through end
 * Returns 1 on success, 0 on error. Sets start/end (1-based).
 */
static int edt_parse_range(const struct edt_buffer *buf, const char *range,
                           int *start, int *end)
{
    if (!range || !range[0] || strcmp(range, ".") == 0) {
        *start = *end = buf->dot;
        return (*start >= 1 && *start <= buf->count);
    }
    if (strcmp(range, "*") == 0) {
        *start = 1;
        *end = buf->count;
        return (buf->count > 0);
    }
    if (strcasecmp(range, "REST") == 0) {
        *start = buf->dot;
        *end = buf->count;
        return (*start >= 1);
    }

    /* Try n:m */
    const char *colon = strchr(range, ':');
    if (colon) {
        *start = (int)strtol(range, NULL, 10);
        *end = (int)strtol(colon + 1, NULL, 10);
    } else {
        *start = *end = (int)strtol(range, NULL, 10);
    }

    if (*start < 1) *start = 1;
    if (*end > buf->count) *end = buf->count;
    return (*start <= *end && *start >= 1);
}

/* Skip whitespace */
static const char *skip_ws(const char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/* Get the next word from the command line, return pointer past it */
static const char *next_word(const char *s, char *word, size_t word_size)
{
    s = skip_ws(s);
    size_t i = 0;
    while (*s && !isspace((unsigned char)*s) && i < word_size - 1)
        word[i++] = *s++;
    word[i] = '\0';
    return s;
}

/*
 * edt_run - Main EDT line-mode editor loop
 *
 * Called from cmd_edit in dcl_builtin.c with the resolved file path.
 * Returns SS$_NORMAL on success.
 */
int edt_run(const char *filepath)
{
    struct edt_buffer buf;
    if (!edt_init(&buf)) {
        fprintf(stderr, "%%EDT-F-INITERR, cannot initialize editor buffer\n");
        return SS$_ABORT;
    }

    strncpy(buf.filename, filepath, sizeof(buf.filename) - 1);
    buf.filename[sizeof(buf.filename) - 1] = '\0';

    /* Try to load existing file */
    if (edt_load(&buf, filepath)) {
        printf("  %d lines read from %s\n", buf.count, filepath);
    } else {
        printf("  Input file does not exist.  Creating new file.\n");
        printf("  [EOB]\n");
    }

    /* Main command loop */
    char cmdline[EDT_MAX_LINE];
    for (;;) {
        printf("*");
        fflush(stdout);

        if (!fgets(cmdline, sizeof(cmdline), stdin))
            break;

        /* Strip trailing newline */
        size_t len = strlen(cmdline);
        if (len > 0 && cmdline[len - 1] == '\n')
            cmdline[len - 1] = '\0';

        /* Skip empty lines */
        const char *p = skip_ws(cmdline);
        if (!*p) continue;

        /* Parse command word */
        char verb[64];
        p = next_word(p, verb, sizeof(verb));

        /* Check if it's a line number */
        if (isdigit((unsigned char)verb[0])) {
            int line_no = (int)strtol(verb, NULL, 10);
            if (line_no >= 1 && line_no <= buf.count) {
                buf.dot = line_no;
                printf("  %d\t%s\n", buf.dot, buf.lines[buf.dot - 1]);
            } else {
                printf("%%EDT-E-INVRANGE, invalid line number %d\n", line_no);
            }
            continue;
        }

        /* Uppercase the verb for matching */
        for (int i = 0; verb[i]; i++)
            verb[i] = toupper((unsigned char)verb[i]);

        /*
         * INSERT (I) - Enter insert mode
         */
        if (strcmp(verb, "INSERT") == 0 || strcmp(verb, "I") == 0) {
            printf("  [Inserting after line %d.  Enter Ctrl+Z or empty line to exit insert mode.]\n",
                   buf.dot);
            char ins_line[EDT_MAX_LINE];
            while (1) {
                if (!fgets(ins_line, sizeof(ins_line), stdin))
                    break;  /* EOF / Ctrl+Z */
                len = strlen(ins_line);
                if (len > 0 && ins_line[len - 1] == '\n')
                    ins_line[len - 1] = '\0';
                if (ins_line[0] == '\0')
                    break;  /* Empty line exits insert mode */
                edt_insert_line(&buf, buf.dot, ins_line);
                buf.dot++;
            }
            printf("  [EOI]\n");
            continue;
        }

        /*
         * TYPE (T) [range] - Display lines
         */
        if (strcmp(verb, "TYPE") == 0 || strcmp(verb, "T") == 0) {
            char range_str[64];
            p = next_word(p, range_str, sizeof(range_str));
            int start, end;
            if (!range_str[0]) {
                /* No range: type current line */
                start = end = buf.dot;
            } else if (!edt_parse_range(&buf, range_str, &start, &end)) {
                printf("%%EDT-E-INVRANGE, invalid range\n");
                continue;
            }
            if (start < 1 || buf.count == 0) {
                printf("%%EDT-E-NOLINES, buffer is empty\n");
                continue;
            }
            for (int i = start; i <= end; i++)
                printf("  %d\t%s\n", i, buf.lines[i - 1]);
            buf.dot = end;
            continue;
        }

        /*
         * DELETE (D) [range] - Delete lines
         */
        if (strcmp(verb, "DELETE") == 0 || strcmp(verb, "D") == 0) {
            char range_str[64];
            p = next_word(p, range_str, sizeof(range_str));
            int start, end;
            if (!range_str[0])
                start = end = buf.dot;
            else if (!edt_parse_range(&buf, range_str, &start, &end)) {
                printf("%%EDT-E-INVRANGE, invalid range\n");
                continue;
            }
            if (start < 1 || start > buf.count) {
                printf("%%EDT-E-NOLINES, no lines to delete\n");
                continue;
            }
            int del_count = end - start + 1;
            for (int i = start - 1; i < end; i++)
                free(buf.lines[i]);
            /* Shift remaining lines up */
            for (int i = start - 1; i < buf.count - del_count; i++)
                buf.lines[i] = buf.lines[i + del_count];
            buf.count -= del_count;
            buf.modified = 1;
            printf("  %d line%s deleted\n", del_count, del_count > 1 ? "s" : "");
            if (buf.dot > buf.count) buf.dot = buf.count;
            if (buf.dot < 1 && buf.count > 0) buf.dot = 1;
            continue;
        }

        /*
         * SUBSTITUTE (S) /old/new/ - Replace text on current line
         */
        if (strcmp(verb, "SUBSTITUTE") == 0 || strcmp(verb, "S") == 0) {
            p = skip_ws(p);
            if (!*p || buf.dot < 1 || buf.dot > buf.count) {
                printf("%%EDT-E-INVSUBST, invalid substitution\n");
                continue;
            }
            char delim = *p++;
            const char *old_start = p;
            const char *old_end = strchr(p, delim);
            if (!old_end) {
                printf("%%EDT-E-INVSUBST, missing delimiter in substitution\n");
                continue;
            }
            size_t old_len = old_end - old_start;
            char old_str[EDT_MAX_LINE];
            if (old_len >= sizeof(old_str)) old_len = sizeof(old_str) - 1;
            memcpy(old_str, old_start, old_len);
            old_str[old_len] = '\0';

            p = old_end + 1;
            const char *new_start = p;
            const char *new_end = strchr(p, delim);
            size_t new_len;
            char new_str[EDT_MAX_LINE];
            if (new_end) {
                new_len = new_end - new_start;
            } else {
                new_len = strlen(new_start);
            }
            if (new_len >= sizeof(new_str)) new_len = sizeof(new_str) - 1;
            memcpy(new_str, new_start, new_len);
            new_str[new_len] = '\0';

            /* Perform substitution on current line */
            char *line = buf.lines[buf.dot - 1];
            char *found = strstr(line, old_str);
            if (!found) {
                printf("%%EDT-E-STRNOTFND, string \"%s\" not found\n", old_str);
                continue;
            }

            char result[EDT_MAX_LINE * 2];
            size_t prefix_len = found - line;
            memcpy(result, line, prefix_len);
            memcpy(result + prefix_len, new_str, new_len);
            strcpy(result + prefix_len + new_len, found + old_len);

            free(buf.lines[buf.dot - 1]);
            buf.lines[buf.dot - 1] = strdup(result);
            buf.modified = 1;
            printf("  %d\t%s\n", buf.dot, buf.lines[buf.dot - 1]);
            continue;
        }

        /*
         * WRITE (W) - Save buffer to file
         */
        if (strcmp(verb, "WRITE") == 0 || strcmp(verb, "W") == 0) {
            if (edt_save(&buf, buf.filename))
                printf("  %d lines written to %s\n", buf.count, buf.filename);
            continue;
        }

        /*
         * EXIT - Save and exit
         */
        if (strcmp(verb, "EXIT") == 0 || strcmp(verb, "EX") == 0) {
            if (buf.modified) {
                if (edt_save(&buf, buf.filename))
                    printf("  %d lines written to %s\n", buf.count, buf.filename);
            }
            edt_free(&buf);
            return SS$_NORMAL;
        }

        /*
         * QUIT - Exit without saving
         */
        if (strcmp(verb, "QUIT") == 0 || strcmp(verb, "Q") == 0) {
            if (buf.modified)
                printf("  [Buffer has been modified; changes discarded]\n");
            edt_free(&buf);
            return SS$_NORMAL;
        }

        /*
         * HELP - Show available commands
         */
        if (strcmp(verb, "HELP") == 0 || strcmp(verb, "H") == 0) {
            printf("  EDT Line-Mode Commands:\n");
            printf("    INSERT (I)             Enter insert mode (end with empty line or Ctrl+Z)\n");
            printf("    TYPE (T) [range]       Display lines\n");
            printf("    DELETE (D) [range]     Delete lines\n");
            printf("    SUBSTITUTE (S) /old/new/  Replace text on current line\n");
            printf("    WRITE (W)              Save buffer to file\n");
            printf("    EXIT (EX)              Save and exit\n");
            printf("    QUIT (Q)               Exit without saving\n");
            printf("    HELP (H)               Show this help\n");
            printf("    <number>               Go to line number\n");
            printf("\n");
            printf("  Ranges: n (line n), n:m (lines n to m), * (all), . (current), REST\n");
            continue;
        }

        printf("%%EDT-E-UNKCMD, unknown command \"%s\"\n", verb);
    }

    /* EOF on input — treat like EXIT */
    if (buf.modified) {
        if (edt_save(&buf, buf.filename))
            printf("  %d lines written to %s\n", buf.count, buf.filename);
    }
    edt_free(&buf);
    return SS$_NORMAL;
}
