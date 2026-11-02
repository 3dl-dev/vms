/*
 * crtl_exec.h - the DEC C RTL's subprocess functions over the VMS executive
 *               (rd vms-e70f). See crtl_exec.c.
 */
#ifndef OVMX_CRTL_EXEC_H
#define OVMX_CRTL_EXEC_H

#include <stddef.h>
#include <stdint.h>

/* exec*() in a vfork context: create the subprocess running `spec` with the
 * ARGUMENTS args[0..nargs) and environment envp. Its VMS PID, or 0 + errno. */
uint32_t ovmx_crtl_exec_create(const char *spec, char *const *args, size_t nargs,
                               char *const *envp);
/* exec*() outside one: wait for `pid` and exit with its completion code. */
void ovmx_crtl_exec_replace(uint32_t pid);

int ovmx_crtl_wait_cond(int pid, uint32_t *cond, int nohang);
int ovmx_crtl_wait_status(uint32_t cond);
int ovmx_crtl_waitpid(int pid, int *status, int options);
int ovmx_crtl_vms_waitpid(int pid, int *status, int options);
int ovmx_crtl_wait(int *status);
int ovmx_crtl_getpid(void);

/* Child side, at image start: the context exec*() left for this process. */
int ovmx_crtl_exec_context(int *argc, char ***argv, char ***envp);

#endif
