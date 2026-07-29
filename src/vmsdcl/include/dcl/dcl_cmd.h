#ifndef __DCL_CMD_H
#define __DCL_CMD_H

/*
 * dcl_cmd.h - Internal header for split DCL command implementations.
 *
 * Shared declarations used by dcl_cmd_show.c, dcl_cmd_set.c,
 * dcl_cmd_file.c, dcl_cmd_process.c, dcl_cmd_io.c, dcl_cmd_misc.c
 * and the dispatch table in dcl_builtin.c.
 */

struct dcl_command;

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
int cmd_tcpip(struct dcl_command *cmd);
int cmd_mount(struct dcl_command *cmd);
int cmd_dismount(struct dcl_command *cmd);
int cmd_edit(struct dcl_command *cmd);
int cmd_attach(struct dcl_command *cmd);
int cmd_convert(struct dcl_command *cmd);
int cmd_install(struct dcl_command *cmd);
int cmd_link(struct dcl_command *cmd);
int cmd_phone(struct dcl_command *cmd);
int cmd_product(struct dcl_command *cmd);

/* ---- Shared helpers (dcl_builtin.c) ---- */

/* VMS month abbreviations */
extern const char *vms_months[];

/* VMS device table */
#define VMS_MAX_DEVICES 64

struct vms_device {
    char vms_name[16];
    char linux_path[256];
    char volume_label[16];
    int  mounted;
};

extern struct vms_device vms_device_table[VMS_MAX_DEVICES];
extern int vms_device_count;

struct vms_device *vms_find_device(const char *name);

/*
 * Check whether a device name (optional trailing colon, case-insensitive) is one of
 * OVMX's fixed, pre-autoconfigured virtual devices — the small inventory MOUNT is
 * allowed to operate on. Real VMS's MOUNT only succeeds against hardware SYSGEN
 * AUTOCONFIGURE already found at boot; OVMX has no physical controllers, so it stands
 * in a small, fixed set of exactly-named devices instead (vms-b9f R2 — a class+unit
 * syntax allowlist was tried first and disproved live against the oracle: MOUNT
 * DBZ0:/DRA0:/MSA0:/$1$DGA0: all matched a "known class, unit 0" pattern but were never
 * modeled by OVMX, and real VMS does not treat "syntactically plausible" as "exists").
 * See docs/design-executive-retrofit.md and vms-b9f. Returns 1 if it is one of OVMX's
 * configured devices, 0 otherwise.
 */
int dcl_is_configured_device(const char *name);

/* Queue initialization helper */
int ensure_queue_init(void);

/* External utility executor */
int dcl_exec_utility(const char *exe_name, const char *facility,
                     char *argv[], int argc);

/* External functions used by command implementations */
extern void dcl_error(const char *facility, int severity, const char *ident,
                      const char *fmt, ...);
extern int dcl_resolve_path(struct dcl_context *ctx, const char *spec,
                            char *linux_path, size_t path_size);
extern int dcl_format_directory(const char *linux_path, char *vms_dir,
                                size_t dir_size);
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
