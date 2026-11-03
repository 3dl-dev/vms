/*
 * symset.c - the CLI-callback RUN target (rd vms-cded).
 *
 * On VMS an image run from DCL that calls LIB$SET_SYMBOL changes DCL's own
 * symbol table, and LIB$GET_SYMBOL reads DCL's symbols. This image reads the
 * DCL symbol OVMX_CLI_IN, then sets a global (OVMX_CLI_GLOBAL) and a local
 * (OVMX_CLI_LOCAL) symbol and exits; tests/qemu/test_run_status_e2e.sh then
 * shows both from DCL. Built and activated exactly like RC3.EXE.
 *
 * If DCL defines OVMX_DOCMD, the image then hands that command to DCL with
 * LIB$DO_COMMAND, which does not return: DCL runs the command after the image
 * exits. A "do_command returned" line therefore means it failed.
 *
 * If DCL defines OVMX_SPIN, the image instead ignores the substrate's terminal
 * stop signal and spins for OVMX_SPIN seconds (rd vms-6d9): CTRL/Y must still
 * interrupt it, as on VMS, because the executive -- not a signal the image can
 * refuse -- stops it; CONTINUE resumes it to "spin ended".
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

struct dsc { uint16_t len; uint8_t dtype, cls; char *ptr; };
extern uint32_t lib$set_symbol(const struct dsc *, const struct dsc *, const uint32_t *);
extern uint32_t lib$get_symbol(const struct dsc *, struct dsc *, uint16_t *, uint32_t *);
extern uint32_t lib$do_command(const struct dsc *);

static struct dsc d(char *s) { struct dsc x = { (uint16_t)strlen(s), 14, 1, s }; return x; }

static int spin(const char *secs)
{
    int n = atoi(secs);
    signal(SIGTSTP, SIG_IGN);
    printf("SYMSET: spinning %d (SIGTSTP ignored)\n", n);
    fflush(stdout);
    for (int i = 0; i < n; i++)
        sleep(1);
    printf("SYMSET: spin ended\n");
    fflush(stdout);
    return 0;
}

int main(void)
{
    char sbuf[16];
    struct dsc sd = { sizeof sbuf - 1, 14, 1, sbuf };
    struct dsc n_s = d("OVMX_SPIN");
    uint16_t slen = 0;
    if (lib$get_symbol(&n_s, &sd, &slen, 0) & 1) {
        sbuf[slen] = '\0';
        return spin(sbuf);
    }

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

    char cbuf[256];
    struct dsc cmd = { sizeof cbuf, 14, 1, cbuf };
    struct dsc n_c = d("OVMX_DOCMD");
    uint16_t clen = 0;
    if (lib$get_symbol(&n_c, &cmd, &clen, 0) & 1) {
        cmd.len = clen;
        st = lib$do_command(&cmd);
        printf("SYMSET: do_command returned %%X%08X\n", st);
        fflush(stdout);
    }
    return 0;
}
