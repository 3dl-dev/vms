#ifndef __DCL_CMD_H
#define __DCL_CMD_H

#include <stdint.h>

/*
 * dcl_cmd.h - Internal header for split DCL command implementations.
 *
 * Shared declarations used by dcl_cmd_show.c, dcl_cmd_set.c,
 * dcl_cmd_file.c, dcl_cmd_process.c, dcl_cmd_io.c, dcl_cmd_misc.c
 * and the dispatch table in dcl_builtin.c.
 */

struct dcl_command;

/*
 * Full VMS privilege name/bit/description table (defined once, in
 * dcl_cmd_show.c). Shared with dcl_lexical.c's F$GETJPI CURPRIV/AUTHPRIV
 * so the list of enforced-privilege NAMES is a byproduct of filtering
 * this ONE canonical table by VMS_PRV_M_ENFORCED (src/kernel/
 * vms_ioctl.h), not a second, hand-maintained list that can silently
 * fall out of step with it (vms-2b8 round 6 -- a value a human must
 * remember to update WILL drift; the fix is to derive it, not to be
 * careful).
 */
struct dcl_priv_name {
    const char *name;
    uint64_t    bit;
    const char *desc;
};
extern const struct dcl_priv_name vms_priv_names[];

/* ---- SHOW commands (dcl_cmd_show.c) ---- */
int cmd_show(struct dcl_command *cmd);

/* ---- SET commands (dcl_cmd_set.c) ---- */
int cmd_set(struct dcl_command *cmd);

/* ---- File operation commands (dcl_cmd_file.c) ---- */
int cmd_directory(struct dcl_command *cmd);
int cmd_type(struct dcl_command *cmd);
int cmd_copy(struct dcl_command *cmd);
int cmd_delete(struct dcl_command *cmd);
int cmd_rename(struct dcl_command *cmd);
int cmd_create(struct dcl_command *cmd);
int cmd_search(struct dcl_command *cmd);
int cmd_purge(struct dcl_command *cmd);
int cmd_append(struct dcl_command *cmd);

/* ---- Process/job commands (dcl_cmd_process.c) ---- */
int cmd_submit(struct dcl_command *cmd);
int cmd_print(struct dcl_command *cmd);
int cmd_spawn(struct dcl_command *cmd);
int cmd_run(struct dcl_command *cmd);
int cmd_wait(struct dcl_command *cmd);
int cmd_stop(struct dcl_command *cmd);
int cmd_exit(struct dcl_command *cmd);
int cmd_logout(struct dcl_command *cmd);
int cmd_pipe(struct dcl_command *cmd);
int cmd_continue(struct dcl_command *cmd);

/* ---- I/O commands (dcl_cmd_io.c) ---- */
int cmd_assign(struct dcl_command *cmd);
int cmd_deassign(struct dcl_command *cmd);
int cmd_define(struct dcl_command *cmd);
int cmd_open(struct dcl_command *cmd);
int cmd_close(struct dcl_command *cmd);
int cmd_read(struct dcl_command *cmd);
int cmd_write(struct dcl_command *cmd);

/* ---- Miscellaneous commands (dcl_cmd_misc.c) ---- */
int cmd_differences(struct dcl_command *cmd);
int cmd_sort(struct dcl_command *cmd);
int cmd_dump(struct dcl_command *cmd);
int cmd_analyze(struct dcl_command *cmd);
int cmd_mail(struct dcl_command *cmd);
int cmd_inquire(struct dcl_command *cmd);
int cmd_monitor(struct dcl_command *cmd);
int cmd_sysgen(struct dcl_command *cmd);
int cmd_sysman(struct dcl_command *cmd);
int cmd_reply(struct dcl_command *cmd);
int cmd_request(struct dcl_command *cmd);
int cmd_accounting(struct dcl_command *cmd);
int cmd_help(struct dcl_command *cmd);
int cmd_recall(struct dcl_command *cmd);

/*
 * DCL command recall buffer (vms-7c7). The interactive command loop records
 * each top-level command line here so RECALL can replay it, independent of any
 * terminal line-editing library. dcl_recall_push() adds one non-empty line;
 * dcl_recall_erase() clears the buffer (RECALL/ERASE). Defined in
 * dcl_cmd_misc.c alongside cmd_recall().
 */
void dcl_recall_push(const char *line);
void dcl_recall_erase(void);

int cmd_tcpip(struct dcl_command *cmd);
int cmd_telnet(struct dcl_command *cmd);   /* TCP/IP client tool (vms-dbb) */
int cmd_ftp(struct dcl_command *cmd);      /* TCP/IP client tool (vms-dbb) */
int cmd_mount(struct dcl_command *cmd);
int cmd_dismount(struct dcl_command *cmd);

/* Shared with dcl_cmd_set.c's cmd_set_volume() (vms-309): the same
 * "is this unit mounted, per the kernel's own /proc/mounts" check
 * MOUNT/DISMOUNT use -- a pure function of the device name, no /dev/vms
 * dependency, so it works identically on host ctest (nothing ever mounted
 * there) and inside a real QEMU boot. */
void mount_point_for_device(const char *log_name, char *buf, size_t sz);
int  mount_point_is_mounted(const char *mount_point);
int cmd_edit(struct dcl_command *cmd);
int cmd_attach(struct dcl_command *cmd);
int cmd_convert(struct dcl_command *cmd);
int cmd_install(struct dcl_command *cmd);
int cmd_initialize(struct dcl_command *cmd);
int cmd_link(struct dcl_command *cmd);
int cmd_phone(struct dcl_command *cmd);
int cmd_product(struct dcl_command *cmd);

/* ---- Shared helpers (dcl_builtin.c) ---- */

/* VMS month abbreviations */
extern const char *vms_months[];

/* Queue initialization helper */
int ensure_queue_init(void);

/* External utility executor */
int dcl_exec_utility(const char *exe_name, const char *facility,
                     char *argv[], int argc);

/*
 * SYS$INPUT-from-procedure (vms-1a9). When an image is activated (RUN, a
 * foreign command, or a DCL utility such as SYSGEN/AUTHORIZE) from WITHIN a
 * command procedure, OpenVMS makes the image's SYS$INPUT the procedure itself:
 * the data lines following the invoking command, up to the next line beginning
 * with '$' (a DCL command) or end-of-file. dcl_sysinput_setup() gathers that
 * block from the innermost procedure stream, repositions the stream to the
 * next '$'-line so the parent DCL resumes there, and redirects fd 0 (which
 * SYS$INPUT resolves to -- sys_assign.c) to the gathered block for the duration
 * of the activation. When DCL is interactive (proc_depth < 0) it is a no-op and
 * SYS$INPUT stays the terminal. dcl_sysinput_restore() puts fd 0 back.
 */
struct dcl_context;
struct dcl_sysinput { int saved_fd0; };
void dcl_sysinput_setup(struct dcl_context *ctx, struct dcl_sysinput *si);
void dcl_sysinput_restore(struct dcl_sysinput *si);

/* P1 control-region establishment (vms-68f.v, in-process image activation).
 *
 * DCL's process-permanent state lives in P1 (the control region), which
 * survives every image activation and rundown -- unlike P0, the per-image
 * program region imgact_activate() maps and tears down. dcl_p1_init() lays a
 * real page-aligned P1 control block at process startup and registers its
 * extent with the executive (vms_kif_p1_map), so $GETJPI reports this
 * process's P1 base/limit -- the wiring the header of vms_kif_p1_map (vms-6f1)
 * had been left waiting for since increment (ii). Idempotent and best-effort:
 * with no /dev/vms the extent is simply not registered (INV-6: no per-process
 * fake), and DCL runs unchanged. Called once from dcl_main().
 * dcl_cmd_process.c owns the block so dcl_activate_image() can hand its
 * protected sub-range straight to imgact_activate(). */
void dcl_p1_init(void);

/* Report DCL's CRITICAL-P1 range -- the crown-jewel sub-range of the P1
 * control block that imgact_activate() mprotect()s read-only while an image
 * runs in User mode (design §A.2.3(b)). Fills *base/*limit and returns 1 when
 * a P1 block was established, 0 otherwise (so dcl_activate_image passes NULL,
 * exactly as before dcl_p1_init ran). */
int dcl_p1_critical_range(uint64_t *base, uint64_t *limit);

/* Image activation, shared by RUN and foreign-command dispatch
 * (dcl_cmd_process.c) */
int dcl_activate_image(struct dcl_context *ctx, const char *display_name,
                       const char *linux_path, char *argv[]);

/* Foreign-command dispatch: a DCL symbol whose value begins with '$'
 * names an image to activate when the symbol is typed bare.
 * symbol_value is the symbol's value with the leading '$' already
 * stripped (dcl_cmd_process.c; called from dcl_exec.c's verb dispatch). */
int dcl_exec_foreign_command(struct dcl_context *ctx, struct dcl_command *cmd,
                             const char *symbol_value);

/* External functions used by command implementations */
extern void dcl_error(const char *facility, int severity, const char *ident,
                      const char *fmt, ...);
extern int dcl_resolve_path(struct dcl_context *ctx, const char *spec,
                            char *linux_path, size_t path_size);
extern int dcl_format_directory(const char *linux_path, char *vms_dir,
                                size_t dir_size);
extern int dcl_directory_header_spec(const char *def, const char *spec,
                                     char *vms_dir, size_t dir_size);
extern int dcl_format_filespec(const char *linux_path, char *vms_spec,
                               size_t spec_size);
extern int dcl_translate_logical(const char *name, char *result,
                                 size_t result_size);
extern int dcl_eval_lexical(struct dcl_context *ctx, const char *expr,
                            char *result, size_t result_size);
extern int dcl_execute_line(const char *line);
extern int dcl_execute_script(const char *filename, int argc, char **argv);
extern int dcl_read_input(struct dcl_context *ctx, const char *prompt,
                          char *buffer, size_t bufsize);

/* VMS filesystem protection functions */
extern int      vmsfs_parse_protection(const char *str, uint16_t *prot);
extern int      vmsfs_format_protection(uint16_t prot, char *buf, size_t bufsize);
extern mode_t   vmsfs_protection_to_mode(uint16_t vms_prot);
extern uint16_t vmsfs_mode_to_protection(mode_t mode);

/* VMS wildcard match */
extern int vmsfs_wildcard_match(const char *pattern, const char *name);

/* VMS ODS-2 name validation */
extern int vmsfs_is_valid_ods2_name(const char *name);

/* vmsfs version management */
extern int vmsfs_purge_versions(const char *linux_dir, const char *basename,
                                const char *ext, int keep_count);
extern int vmsfs_list_versions(const char *linux_dir, const char *basename,
                               const char *ext, int *versions, int max_versions,
                               int *count);

#endif /* __DCL_CMD_H */
