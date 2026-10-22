/*
 * test_syssvc_lib_do_command.c - LIB$DO_COMMAND in the booted guest (LIB$ Manual:
 * "stops program execution and directs the command language interpreter (CLI) to
 * execute a command ... If successful, LIB$DO_COMMAND does not return control to the
 * calling program").
 *
 * This suite plays DCL's side of the CLI callback channel a fork()+execve() image
 * reaches DCL by (OVMX$CLI_FD, src/libvms/include/ovmx_cli.h) and runs the image
 * side in a child:
 *   - with no CLI the call returns LIB$_NOCLI;
 *   - the CLI receives OVMX_CLI_OP_DO_COMMAND carrying the command text;
 *   - when the CLI accepts, the image exits with SS$_NORMAL and never returns;
 *   - when the CLI refuses, the refusal is returned to the caller.
 * (DCL executing the command after the image exits is proven end to end by
 * tests/qemu/test_run_status_e2e.sh, RUN SYS$SYSTEM:SYMSET.EXE with OVMX_DOCMD.)
 *
 * No /dev/vms -> honest SKIP (77).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "vms_kif.h"
#include "ssdef.h"
#include "descrip.h"
#include "libdef.h"
#include "lib$routines.h"
#include "ovmx_cli.h"

#define EXIT_SKIP 77
#define CMDTEXT "WRITE SYS$OUTPUT \"DOCMD-RAN\""

static int pass, fail;
static void check(int c, const char *m)
{
    if (c) { printf("  PASS: %s\n", m); pass++; }
    else   { printf("  FAIL: %s\n", m); fail++; }
}

struct outcome {
    int      got_req;            /* the CLI side read a request */
    struct ovmx_cli_req req;
    int      returned;           /* the image side came back from the call */
    uint32_t status;             /* ...with this status */
    int      exit_code;          /* the image's exit code (-1: not a normal exit) */
};

/* Run lib$do_command in a child. `cli` 0: no CLI; else answer with `answer`. */
static void run_case(int cli, uint32_t answer, struct outcome *o)
{
    int sv[2] = { -1, -1 }, rp[2];
    memset(o, 0, sizeof *o);
    o->exit_code = -1;
    if (cli && socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0)
        return;
    if (pipe(rp) != 0)
        return;
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        char nb[16];
        struct dsc$descriptor_s d = { sizeof CMDTEXT - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S,
                                      (char *)CMDTEXT };
        close(rp[0]);
        if (cli) {
            close(sv[0]);
            snprintf(nb, sizeof nb, "%d", sv[1]);
            setenv(OVMX_CLI_FD_ENV, nb, 1);
        } else {
            unsetenv(OVMX_CLI_FD_ENV);
        }
        uint32_t st = lib$do_command(&d);
        (void)!write(rp[1], &st, sizeof st);      /* reached only if it returned */
        _exit(9);
    }
    close(rp[1]);
    if (cli) {
        struct ovmx_cli_rsp rsp;
        close(sv[1]);
        if (read(sv[0], &o->req, sizeof o->req) == (ssize_t)sizeof o->req) {
            o->got_req = 1;
            memset(&rsp, 0, sizeof rsp);
            rsp.status = answer;
            (void)!write(sv[0], &rsp, sizeof rsp);
        }
    }
    if (read(rp[0], &o->status, sizeof o->status) == (ssize_t)sizeof o->status)
        o->returned = 1;
    close(rp[0]);
    if (cli)
        close(sv[0]);
    int st = 0;
    if (waitpid(pid, &st, 0) == pid && WIFEXITED(st))
        o->exit_code = WEXITSTATUS(st);
}

int main(void)
{
    struct outcome o;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_lib_do_command: LIB$DO_COMMAND ===\n");
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_lib_do_command: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    run_case(0, 0, &o);
    check(o.returned && o.status == LIB$_NOCLI, "with no CLI LIB$DO_COMMAND returns LIB$_NOCLI");

    run_case(1, SS$_NORMAL, &o);
    check(o.got_req && o.req.op == OVMX_CLI_OP_DO_COMMAND &&
          o.req.value_len == sizeof CMDTEXT - 1 &&
          memcmp(o.req.value, CMDTEXT, sizeof CMDTEXT - 1) == 0,
          "the CLI receives the command text as a DO_COMMAND request");
    /* negctl: lib-do-command-returns */
    check(!o.returned && o.exit_code == 0,
          "an accepted command does not return: the image exits with SS$_NORMAL");

    run_case(1, LIB$_INSCLIMEM, &o);
    check(o.got_req && o.returned && o.status == LIB$_INSCLIMEM,
          "a command the CLI refuses returns the CLI's status to the caller");

    printf("=== test_syssvc_lib_do_command: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
