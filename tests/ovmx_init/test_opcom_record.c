/*
 * test_opcom_record.c - OPCOM's records are the real VAX's, byte for byte
 * (rd vms-798).
 *
 * Every line OPCOM.EXE builds (src/ovmx_opcom/opcom_record.h) is checked
 * against the real OpenVMS VAX V7.3 console in the demo, read from the capture
 * itself (tests/lab/captures/vms-c56-console-chatter-20261021/C.console.txt),
 * with the same node, user and time the VAX printed -- so a format OPCOM uses
 * that a real VAX does not is a failure here, not a review comment.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opcom_record.h"

#ifndef OVMX_SOURCE_DIR
#error "OVMX_SOURCE_DIR must name the repository root"
#endif

static int failures;

static char *slurp(const char *rel)
{
    char path[1024];
    FILE *f;
    long n;
    char *b;

    snprintf(path, sizeof path, "%s/%s", OVMX_SOURCE_DIR, rel);
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    b[n] = '\0';
    return b;
}

/* Is `line` a whole line of the capture? */
static int has_line(const char *cap, const char *line)
{
    size_t n = strlen(line);
    const char *p = cap;

    while ((p = strstr(p, line)) != NULL) {
        int bol = (p == cap || p[-1] == '\n');
        char e = p[n];
        if (bol && (e == '\r' || e == '\n' || e == '\0'))
            return 1;
        p++;
    }
    return 0;
}

static void check(const char *cap, const char *line, const char *what)
{
    if (has_line(cap, line)) {
        printf("  ok   %s: \"%s\"\n", what, line);
    } else {
        printf("  FAIL %s: \"%s\" is not a line of the real VAX's console\n", what, line);
        failures++;
    }
}

int main(void)
{
    char *cap = slurp("tests/lab/captures/vms-c56-console-chatter-20261021/C.console.txt");
    char buf[256];
    unsigned i;

    printf("=== test_opcom_record (rd vms-798) ===\n");
    if (!cap) {
        printf("  FAIL the oracle capture is not readable\n");
        return 1;
    }

    opcom_banner(buf, sizeof buf, "21-OCT-2026 15:07:10.69");
    check(cap, buf, "banner");
    opcom_enabled_line(buf, sizeof buf, "VAXC", "OPA0", "SYSTEM");
    check(cap, buf, "operator enabled");
    opcom_status_line(buf, sizeof buf, "VAXC", "OPA0");
    check(cap, buf, "operator status");
    for (i = 0; i < OPCOM_N_CLASS_LINES; i++)
        check(cap, opcom_all_classes[i], "class list");
    opcom_from_line(buf, sizeof buf, 0, 0, "JOB_CONTROL", "VAXC");
    check(cap, buf, "message from user");

    /* The numbered form, whose oracle is docs/design-opcom-executive-logging.md
     * sec2 (lab-Alpha): "Request <N>, from user <U> on <N>". */
    opcom_from_line(buf, sizeof buf, 1, 7, "SYSTEM", "ALPHA1");
    if (strcmp(buf, "Request 7, from user SYSTEM on ALPHA1") == 0) {
        printf("  ok   numbered request: \"%s\"\n", buf);
    } else {
        printf("  FAIL numbered request: \"%s\"\n", buf);
        failures++;
    }

    {   /* the text of an OPC block is taken by length and trimmed */
        const char blk[] = "\x03\x01\x00\x00\x00\x00\x00\x00" "DECnet starting\r\n";
        char out[64];
        size_t n = opcom_block_text(blk, sizeof blk - 1, 8, out, sizeof out);
        if (n == 15 && strcmp(out, "DECnet starting") == 0) {
            printf("  ok   block text by length, CR/LF trimmed\n");
        } else {
            printf("  FAIL block text: %zu \"%s\"\n", n, out);
            failures++;
        }
        if (opcom_block_text(blk, 8, 8, out, sizeof out) == 0 && out[0] == '\0') {
            printf("  ok   a header-only block carries no text\n");
        } else {
            printf("  FAIL header-only block\n");
            failures++;
        }
    }

    free(cap);
    printf("=== test_opcom_record: %d failed ===\n", failures);
    return failures ? 1 : 0;
}
