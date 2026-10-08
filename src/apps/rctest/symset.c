/*
 * symset.c - the CLI-callback RUN target (rd vms-cded).
 *
 * On VMS an image run from DCL that calls LIB$SET_SYMBOL changes DCL's own
 * symbol table, and LIB$GET_SYMBOL reads DCL's symbols. This image reads the
 * DCL symbol OVMX_CLI_IN, then sets a global (OVMX_CLI_GLOBAL) and a local
 * (OVMX_CLI_LOCAL) symbol and exits; tests/qemu/test_run_status_e2e.sh then
 * shows both from DCL. Built and activated exactly like RC3.EXE.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

struct dsc { uint16_t len; uint8_t dtype, cls; char *ptr; };
extern uint32_t lib$set_symbol(const struct dsc *, const struct dsc *, const uint32_t *);
extern uint32_t lib$get_symbol(const struct dsc *, struct dsc *, uint16_t *, uint32_t *);

static struct dsc d(char *s) { struct dsc x = { (uint16_t)strlen(s), 14, 1, s }; return x; }

int main(void)
{
    char inbuf[128];
    struct dsc in = { sizeof inbuf, 14, 1, inbuf };
    struct dsc n_in = d("OVMX_CLI_IN");
    uint16_t len = 0;
    uint32_t table = 0;
    uint32_t st = lib$get_symbol(&n_in, &in, &len, &table);
    if (st & 1)
        printf("SYMSET: OVMX_CLI_IN=\"%.*s\" table=%u\n", (int)len, inbuf, table);
    else
        printf("SYMSET: OVMX_CLI_IN not found, status %%X%08X\n", st);

    uint32_t glob = 2, loc = 1;
    struct dsc n_g = d("OVMX_CLI_GLOBAL"), v_g = d("set by image");
    struct dsc n_l = d("OVMX_CLI_LOCAL"), v_l = d("local from image");
    printf("SYMSET: set global %%X%08X local %%X%08X\n",
           lib$set_symbol(&n_g, &v_g, &glob), lib$set_symbol(&n_l, &v_l, &loc));
    fflush(stdout);
    return 0;
}
