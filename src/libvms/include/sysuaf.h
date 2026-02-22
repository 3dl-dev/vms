/*
 * sysuaf.h - SYSUAF (System User Authorization File) library
 *
 * Shared interface for looking up users and authenticating passwords
 * against SYS$SYSTEM:SYSUAF.DAT. Used by vms_login, vms_ssh_auth,
 * and vmssshd.
 *
 * The library is usable without any VMS runtime being initialized —
 * it is pure file parsing and SHA256 hashing.
 */

#ifndef SYSUAF_H
#define SYSUAF_H

#include <stdint.h>
#include "ovmx_layout.h"

#define SYSUAF_PATH VMS_SYSUAF_PATH

typedef struct {
    char     username[64];
    char     password_hash[128];
    uint32_t uic_group;
    uint32_t uic_member;
    char     default_dir[256];
    char     flags[64];
    char     privileges[256];
} sysuaf_record_t;

/* Look up a user in sysuaf.dat. Returns 0 on success, -1 if not found. */
int sysuaf_lookup(const char *username, sysuaf_record_t *rec);

/* Authenticate: returns 1 if password matches, 0 if not.
   Empty hash = no password required (returns 1). */
int sysuaf_authenticate(const sysuaf_record_t *rec, const char *password);

/* Parse VMS privilege string (e.g. "TMPMBX,NETMBX,OPER") into bitmask */
uint64_t sysuaf_parse_privileges(const char *priv_string);

#endif /* SYSUAF_H */
