/*
 * lib_misc.c - Miscellaneous LIB$ Routines
 *
 * Provides simplified wrappers around sys$getjpi and sys$getsyi,
 * and the lib$spawn routine for subprocess creation.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"
#include "prcdef.h"
#include "lnmdef.h"

/* Imported from starlet.h via forward declarations */
extern uint32_t sys$getjpiw(uint32_t efn, const uint32_t *pidadr,
                             const struct dsc$descriptor_s *prcnam,
                             const struct item_list_3 *itmlst,
                             void *iosb,
                             void (*astadr)(uint32_t), uint32_t astprm);
extern uint32_t sys$getsyiw(uint32_t efn, const uint32_t *csidadr,
                              const struct dsc$descriptor_s *nodename,
                              const struct item_list_3 *itmlst,
                              void *iosb,
                              void (*astadr)(uint32_t), uint32_t astprm);

/*
 * lib$getjpi - Get Job/Process Information (simplified wrapper).
 *
 * Provides a simpler calling interface to sys$getjpi for retrieving
 * a single item. Either result (for numeric items) or result_str
 * (for string items) should be provided.
 *
 * Parameters:
 *   item_code  - JPI$_ item code
 *   pid        - Process ID (or NULL for current process)
 *   prcnam     - Process name descriptor (or NULL)
 *   result     - Receives numeric result (or NULL)
 *   result_str - Receives string result (or NULL)
 *   result_len - Receives actual length of result (or NULL)
 */
uint32_t lib$getjpi(const uint32_t *item_code, const uint32_t *pid,
                    const struct dsc$descriptor_s *prcnam,
                    void *result, struct dsc$descriptor_s *result_str,
                    uint16_t *result_len) {
    if (!item_code) return SS$_BADPARAM;

    struct item_list_3 items[2];
    memset(items, 0, sizeof(items));

    items[0].buflen = result_str ? result_str->dsc$w_length : sizeof(uint32_t);
    items[0].item_code = (uint16_t)*item_code;
    items[0].bufaddr = result_str ? (void *)result_str->dsc$a_pointer
                                  : (void *)result;
    items[0].retlen = result_len;
    /* Terminator */
    items[1].buflen = 0;
    items[1].item_code = 0;
    items[1].bufaddr = NULL;
    items[1].retlen = NULL;

    return sys$getjpiw(0, pid, prcnam, items, NULL, NULL, 0);
}

/*
 * lib$getsyi - Get System Information (simplified wrapper).
 *
 * Provides a simpler calling interface to sys$getsyi for retrieving
 * a single item.
 *
 * Parameters:
 *   item_code  - SYI$_ item code
 *   result     - Receives numeric result (or NULL)
 *   result_str - Receives string result (or NULL)
 *   result_len - Receives actual length (or NULL)
 *   csid       - Cluster system ID (or NULL)
 *   node       - Node name descriptor (or NULL)
 */
uint32_t lib$getsyi(const uint32_t *item_code,
                    void *result, struct dsc$descriptor_s *result_str,
                    uint16_t *result_len, const uint32_t *csid,
                    const struct dsc$descriptor_s *node) {
    if (!item_code) return SS$_BADPARAM;

    struct item_list_3 items[2];
    memset(items, 0, sizeof(items));

    items[0].buflen = result_str ? result_str->dsc$w_length : sizeof(uint32_t);
    items[0].item_code = (uint16_t)*item_code;
    items[0].bufaddr = result_str ? (void *)result_str->dsc$a_pointer
                                  : (void *)result;
    items[0].retlen = result_len;
    /* Terminator */
    items[1].buflen = 0;
    items[1].item_code = 0;
    items[1].bufaddr = NULL;
    items[1].retlen = NULL;

    return sys$getsyiw(0, csid, node, items, NULL, NULL, 0);
}

/*
 * lib$spawn - Spawn a subprocess.
 *
 * Creates a subprocess to execute the given command. If command is
 * NULL, spawns an interactive shell. Waits for completion and
 * returns the subprocess exit status.
 *
 * Parameters:
 *   command     - Command string descriptor (or NULL for interactive)
 *   input_file  - SYS$INPUT redirection (or NULL)
 *   output_file - SYS$OUTPUT redirection (or NULL)
 *   flags       - Spawn flags (ignored)
 *   prcnam      - Subprocess name (ignored)
 *   pid         - Receives subprocess PID (or NULL)
 *   status      - Receives exit status (or NULL)
 *   efn         - Event flag (ignored)
 *   astadr      - AST routine (ignored)
 *   astprm      - AST parameter (ignored)
 *   prompt      - Prompt string (ignored)
 *   cli_name    - CLI name (ignored)
 *   table_name  - CLI table name (ignored)
 */
uint32_t lib$spawn(const struct dsc$descriptor_s *command,
                   const struct dsc$descriptor_s *input_file,
                   const struct dsc$descriptor_s *output_file,
                   const uint32_t *flags,
                   const struct dsc$descriptor_s *prcnam,
                   uint32_t *pid, uint32_t *status,
                   const uint32_t *efn,
                   void *astadr,
                   void *astprm,
                   const struct dsc$descriptor_s *prompt,
                   const struct dsc$descriptor_s *cli_name,
                   const struct dsc$descriptor_s *table_name) {
    (void)flags; (void)prcnam; (void)efn; (void)astadr; (void)astprm;
    (void)prompt; (void)cli_name; (void)table_name;

    char cmd[4096] = "";
    char *argv[4];

    if (command && command->dsc$a_pointer && command->dsc$w_length > 0) {
        dsc$strncpy(cmd, command, sizeof(cmd));
        argv[0] = "/bin/sh";
        argv[1] = "-c";
        argv[2] = cmd;
        argv[3] = NULL;
    } else {
        /* No command = spawn interactive shell */
        argv[0] = "/bin/sh";
        argv[1] = NULL;
        argv[2] = NULL;
        argv[3] = NULL;
    }

    pid_t child = fork();
    if (child < 0) return SS$_INSFMEM;

    if (child == 0) {
        /* Child process - set up I/O redirection */
        if (input_file && input_file->dsc$a_pointer) {
            char path[256];
            dsc$strncpy(path, input_file, sizeof(path));
            FILE *f = freopen(path, "r", stdin);
            (void)f;
        }
        if (output_file && output_file->dsc$a_pointer) {
            char path[256];
            dsc$strncpy(path, output_file, sizeof(path));
            FILE *f = freopen(path, "w", stdout);
            (void)f;
        }

        if (command && command->dsc$a_pointer && command->dsc$w_length > 0) {
            execv("/bin/sh", argv);
        } else {
            execl("/bin/sh", "/bin/sh", (char *)NULL);
        }
        _exit(1);
    }

    /* Parent process */
    if (pid) *pid = (uint32_t)child;

    /* Wait for child to complete */
    int wstatus;
    waitpid(child, &wstatus, 0);
    if (status) {
        *status = WIFEXITED(wstatus) ? (uint32_t)WEXITSTATUS(wstatus) : SS$_ABORT;
    }

    return SS$_NORMAL;
}
