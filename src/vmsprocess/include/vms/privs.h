/*
 * privs.h - VMS Privilege Name-to-Bitmask Parsing
 *
 * Shared header providing parse_privilege_string() as a static inline
 * function. Used by dcl_main.c, vms_login.c, and sys_process.c to
 * convert comma-separated privilege name strings into uint64_t bitmasks.
 *
 * Privilege bit assignments match the VMS convention:
 *   Bit 0:  TMPMBX   - Create temporary mailbox
 *   Bit 1:  NETMBX   - Create network device
 *   Bit 2:  OPER     - Operator privilege
 *   Bit 3:  SYSPRV   - System privilege
 *   Bit 4:  BYPASS   - Bypass all protection
 *   Bit 5:  SETPRV   - Set any privilege
 *   Bit 6:  CMKRNL   - Change mode to kernel
 *   Bit 7:  CMEXEC   - Change mode to executive
 *   Bit 8:  SYSNAM   - Insert system logical names
 *   Bit 9:  GRPNAM   - Insert group logical names
 *   Bit 10: DETACH   - Create detached processes
 *   Bit 11: ALTPRI   - Alter process priority
 *   Bit 12: WORLD    - Affect other processes in the world
 *   Bit 13: GROUP    - Affect other processes in the group
 *   Bit 14: LOG_IO   - Logical I/O
 *   Bit 15: PHY_IO   - Physical I/O
 */

#ifndef __VMS_PRIVS_H
#define __VMS_PRIVS_H

#include <stdint.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */

/* Individual privilege bits */
#define PRV$M_TMPMBX    (1ULL << 0)
#define PRV$M_NETMBX    (1ULL << 1)
#define PRV$M_OPER      (1ULL << 2)
#define PRV$M_SYSPRV    (1ULL << 3)
#define PRV$M_BYPASS    (1ULL << 4)
#define PRV$M_SETPRV    (1ULL << 5)
#define PRV$M_CMKRNL    (1ULL << 6)
#define PRV$M_CMEXEC    (1ULL << 7)
#define PRV$M_SYSNAM    (1ULL << 8)
#define PRV$M_GRPNAM    (1ULL << 9)
#define PRV$M_DETACH    (1ULL << 10)
#define PRV$M_ALTPRI    (1ULL << 11)
#define PRV$M_WORLD     (1ULL << 12)
#define PRV$M_GROUP     (1ULL << 13)
#define PRV$M_LOG_IO    (1ULL << 14)
#define PRV$M_PHY_IO    (1ULL << 15)

/* All privileges */
#define PRV$M_ALL       0xFFFFFFFFFFFFFFFFULL

/*
 * Parse a comma-separated privilege string into a bitmask.
 * Supports: ALL, TMPMBX, NETMBX, OPER, SYSPRV, etc.
 * Returns 0 for NULL/empty input.
 */
static inline uint64_t parse_privilege_string(const char *str)
{
    if (!str || !str[0]) return 0;

    static const struct {
        const char *name;
        uint64_t    bit;
    } priv_table[] = {
        { "TMPMBX",   PRV$M_TMPMBX },
        { "NETMBX",   PRV$M_NETMBX },
        { "OPER",     PRV$M_OPER },
        { "SYSPRV",   PRV$M_SYSPRV },
        { "BYPASS",   PRV$M_BYPASS },
        { "SETPRV",   PRV$M_SETPRV },
        { "CMKRNL",   PRV$M_CMKRNL },
        { "CMEXEC",   PRV$M_CMEXEC },
        { "SYSNAM",   PRV$M_SYSNAM },
        { "GRPNAM",   PRV$M_GRPNAM },
        { "DETACH",   PRV$M_DETACH },
        { "ALTPRI",   PRV$M_ALTPRI },
        { "WORLD",    PRV$M_WORLD },
        { "GROUP",    PRV$M_GROUP },
        { "LOG_IO",   PRV$M_LOG_IO },
        { "PHY_IO",   PRV$M_PHY_IO },
        { NULL, 0 }
    };

    if (strcasecmp(str, "ALL") == 0)
        return PRV$M_ALL;

    uint64_t mask = 0;
    char buf[512];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(buf, ",", &saveptr);
    while (token) {
        /* Trim leading spaces */
        while (*token == ' ') token++;

        if (strcasecmp(token, "ALL") == 0)
            return PRV$M_ALL;

        for (int i = 0; priv_table[i].name; i++) {
            if (strcasecmp(token, priv_table[i].name) == 0) {
                mask |= priv_table[i].bit;
                break;
            }
        }
        token = strtok_r(NULL, ",", &saveptr);
    }

    return mask;
}

#endif /* __VMS_PRIVS_H */
