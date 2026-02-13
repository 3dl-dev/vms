#ifndef __VMS_PROCESS_H
#define __VMS_PROCESS_H

#include <stdint.h>
#include <sys/types.h>

/* Process status flags */
#define PRC_M_DETACH     0x01
#define PRC_M_NOACNT     0x02
#define PRC_M_BATCH      0x04
#define PRC_M_HIBER      0x08
#define PRC_M_INTER      0x80

/* VMS process info */
typedef struct {
    pid_t    linux_pid;
    uint32_t vms_pid;        /* VMS-style PID (generated) */
    char     prcnam[16];     /* Process name */
    char     username[32];
    uint32_t uic;            /* [group,member] packed */
    uint32_t base_priority;
    uint32_t flags;
    uint32_t status;         /* Current process status */
} vms_process_t;

/* Get current process info */
vms_process_t *vms_get_current_process(void);

/* Convert Linux PID to VMS-style PID (hex format) */
uint32_t vms_pid_from_linux(pid_t pid);

/* Format UIC as [group,member] string */
void vms_format_uic(uint32_t uic, char *buf, size_t bufsize);

/* Parse UIC from [group,member] string */
uint32_t vms_parse_uic(const char *str);

#endif
