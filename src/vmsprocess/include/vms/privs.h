/*
 * privs.h - VMS Privilege Name-to-Bitmask Parsing
 *
 * Shared header providing parse_privilege_string() as a static inline
 * function. Used by dcl_main.c, vms_login.c, and sys_process.c to
 * convert comma-separated privilege name strings into uint64_t bitmasks.
 *
 * Privilege bit assignments match real OpenVMS Alpha values
 * (see prvdef.h for the full authoritative set).
 */

#ifndef __VMS_PRIVS_H
#define __VMS_PRIVS_H

#include <string.h>
#include <strings.h>   /* strcasecmp */
#include "prvdef.h"    /* Single source of truth for PRV$M_* bit positions */

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
        { "SETPRI",   PRV$M_SETPRI },
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
