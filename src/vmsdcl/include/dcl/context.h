#ifndef __DCL_CONTEXT_H
#define __DCL_CONTEXT_H

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include "dcl/terminal.h"

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

    /* Command procedure stack */
    struct {
        FILE *fp;
        char filename[256];
        int  line_number;
        /* Per-level DCL error control (ON / SET [NO]ON). Each command level
         * (an @ procedure or a CALLed subroutine) carries its own state; a new
         * level starts at the defaults below (memset on push). Clean-room
         * (Rule 8): VSI OpenVMS DCL Dictionary — "ON", "SET ON"/"SET NOON";
         * OpenVMS User's Manual — "Controlling Error Conditions". */
        int  noon;          /* SET NOON at this level: 1 = DCL does NOT check
                             * $STATUS after a command (no default exit, no ON
                             * action). 0 (SET ON, the default) restores it. */
        int  on_armed;      /* 1 = an ON action is armed at this level (one-shot);
                             * 0 = default action (exit on ERROR/SEVERE). */
        int  on_severity;   /* severity that triggers the armed action:
                             * 0=WARNING, 2=ERROR (ON default), 4=SEVERE_ERROR */
        char on_action[256];/* THEN command run when triggered (e.g. "GOTO ERR",
                             * "EXIT"); "CONTINUE" means resume past the error */
        char params[8][256]; /* P1-P8 parameters */
        int  is_subroutine;  /* 1 = this level is a CALLed SUBROUTINE block */
        int  gosub_base;     /* gosub_depth on entry: RETURN below this ends
                              * the level (subroutine/procedure), not a GOSUB */
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

    /* SET NOON / SET ON — suppress error handler */
    int noon_active;          /* 1 = NOON (suppress ON ERROR handler) */

    /* Interactive vs batch */
    int interactive;
    int exit_requested;
    int logout_requested;
    int exit_status;

    /* RETURN from a CALLed SUBROUTINE (distinct from GOSUB RETURN, which
     * unwinds within the same level via gosub_stack). The subroutine loop in
     * dcl_script.c checks return_requested and stops; return_status carries the
     * optional status argument, or $STATUS when RETURN gives none. */
    int return_requested;
    int return_status;

    /* Control-Y handling */
    int ctrl_y_enabled;
    pid_t interrupted_pid;    /* PID of Ctrl-Y stopped child (0 = none) */

    /* Control-T handling (SET CONTROL=T / SET NOCONTROL=T). VMS default is
     * DISABLED (OpenVMS User's Manual, "Interrupting Command Execution":
     * "Ctrl/T is disabled by default"), so 0 == off until SET CONTROL=T. */
    int ctrl_t_enabled;

    /* User info */
    char username[64];
    char process_name[16];

    /* Multi-user context (set from VMS_* env vars by vms_login) */
    uint32_t uic_group;
    uint32_t uic_member;
    uint64_t privileges;
    uint16_t default_protection;
    int logged_in;

    /* Open file channels (OPEN command).
     *
     * vms-5f0 (epic vms-208, atomic flip): a channel opened on a REAL file
     * rides RMS ($OPEN/$GET/$CREATE/$PUT over the Files-11 ODS-2 ACP), not a
     * stdio fopen() on a vmsfs_to_linux_path passthrough -- the passthrough
     * cannot see files that live only on the genuine ODS-2 SYS$DISK
     * (SYS$STARTUP:VMS$PHASES.DAT et al). `reader`/`writer` hold the RMS
     * handle; `fp` is kept ONLY for the SYS$OUTPUT:/SYS$ERROR:/SYS$INPUT:
     * standard-stream channels, which are process streams, not RMS files.
     * Exactly one of {fp, reader, writer} is non-NULL on an open channel. */
    struct {
        FILE *fp;
        struct dcl_rms_reader *reader;
        struct dcl_rms_writer *writer;
        char name[64];
        int  mode;  /* 0=read, 1=write, 2=append */
    } channels[16];

    /* SET MESSAGE flags (1 = show, 0 = suppress) */
    int msg_facility;
    int msg_severity;
    int msg_ident;
    int msg_text;

    /* SET TERMINAL settings — full VMS characteristics model */
    struct vms_terminal terminal;

    /* SET PROCESS settings */
    int process_priority;

    /* SET WORKING_SET settings */
    int ws_quota;

    /* SET AUDIT flag. SET ACCOUNTING's flag is NOT here (vms-17d,
     * INV-DCL): accounting-enabled is a real, persisted, system-wide
     * state (ovmx_accounting_is_enabled()/_set_enabled(),
     * src/libvms/rtl/ovmx_accounting.c), not per-DCL-context. */
    int audit_enabled;
};

/* Global context accessor */
struct dcl_context *dcl_get_context(void);

/* Initialize context */
void dcl_context_init(struct dcl_context *ctx);

#endif /* __DCL_CONTEXT_H */
