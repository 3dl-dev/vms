#ifndef __DCL_CONTEXT_H
#define __DCL_CONTEXT_H

#include <stdint.h>
#include <stdio.h>

#define DCL_MAX_NEST    32     /* Max procedure nesting depth */
#define DCL_MAX_PROMPT  64

/* Verification modes */
#define DCL_VERIFY_OFF     0
#define DCL_VERIFY_ON      1
#define DCL_VERIFY_IMAGE   2

/* DCL session context */
struct dcl_context {
    /* Prompt */
    char prompt[DCL_MAX_PROMPT];

    /* Default directory (VMS-style) */
    char default_dir[512];
    /* Default directory (Linux path) */
    char default_linux[512];

    /* Command procedure stack */
    struct {
        FILE *fp;
        char filename[256];
        int  line_number;
        int  on_error;      /* ON ERROR action: 0=none, 1=continue, 2=goto */
        int  on_severe;     /* ON SEVERE_ERROR action */
        char on_error_label[256];
        char on_severe_label[256];
        char params[8][256]; /* P1-P8 parameters */
    } proc_stack[DCL_MAX_NEST];
    int proc_depth;

    /* GOSUB return stack */
    struct {
        long file_offset;
        int  line_number;
    } gosub_stack[DCL_MAX_NEST];
    int gosub_depth;

    /* IF block nesting */
    struct {
        int in_if;
        int condition_true;
        int in_else;
        int skip;
    } if_stack[DCL_MAX_NEST];
    int if_depth;

    /* Status */
    uint32_t last_status;     /* $STATUS */
    uint32_t last_severity;   /* $SEVERITY */

    /* Verification */
    int verify;               /* Current verify mode */

    /* ON ERROR handling (interactive level) */
    int on_error_continue;    /* ON ERROR THEN CONTINUE */
    int on_error_goto;        /* ON ERROR THEN GOTO label */
    char on_error_label[256];

    /* Interactive vs batch */
    int interactive;
    int exit_requested;
    int logout_requested;
    int exit_status;

    /* Control-Y handling */
    int ctrl_y_enabled;

    /* User info */
    char username[64];
    char process_name[16];

    /* Multi-user context (set from VMS_* env vars by vms_login) */
    uint32_t uic_group;
    uint32_t uic_member;
    uint64_t privileges;
    uint16_t default_protection;
    int logged_in;

    /* Open file channels (OPEN command) */
    struct {
        FILE *fp;
        char name[64];
        int  mode;  /* 0=read, 1=write, 2=append */
    } channels[16];
};

/* Global context accessor */
struct dcl_context *dcl_get_context(void);

/* Initialize context */
void dcl_context_init(struct dcl_context *ctx);

#endif /* __DCL_CONTEXT_H */
