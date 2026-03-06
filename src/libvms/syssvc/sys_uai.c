/*
 * sys_uai.c - User Authorization File System Services
 *
 * Implements sys$getuai and sys$setuai for querying and updating
 * user account information from /etc/ovmx/sysuaf.dat.
 *
 * The sysuaf.dat format is colon-delimited text:
 *   USERNAME:PASSWORD_HASH:UIC_GROUP:UIC_MEMBER:DEFAULT_DIR:FLAGS:PRIVILEGES
 *
 * sys$getuai fills in requested UAI$_ items from the matching record.
 * sys$setuai updates the record in place (requires SYSPRV privilege).
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include "starlet.h"
#include "uaidef.h"
#include "prvdef.h"
#include "vms/pcb.h"

/* Path to the system authorization file */
#include "ovmx_layout.h"
#include "vmsfs/filespec.h"
#define SYSUAF_PATH VMS_SYSUAF_PATH

/* Maximum line length in sysuaf.dat */
#define SYSUAF_LINE_MAX 512

/*
 * Parsed UAF record — fields extracted from one sysuaf.dat line.
 */
struct uaf_record {
    char username[32];
    char password_hash[128];
    unsigned int uic_group;
    unsigned int uic_member;
    char default_dir[256];
    uint32_t flags;
    char privileges[256];   /* comma-separated privilege names */
};

/*
 * Parse privilege name list into a 64-bit mask.
 *
 * Supported privilege names match the common VMS set:
 *   ALL, OPER, SYSPRV, TMPMBX, NETMBX, BYPASS, SYSLCK, SYSNAM, CMKRNL
 */
static uint64_t parse_privileges(const char *privstr)
{
    uint64_t mask = 0;

    if (strcmp(privstr, "ALL") == 0) {
        return ~(uint64_t)0;
    }

    /* Walk comma-separated tokens (thread-safe) */
    char buf[256];
    strncpy(buf, privstr, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);
    while (tok) {
        /* Trim leading whitespace */
        while (*tok == ' ') tok++;

        if      (strcmp(tok, "OPER")    == 0) mask |= PRV$M_OPER;
        else if (strcmp(tok, "SYSPRV")  == 0) mask |= PRV$M_SYSPRV;
        else if (strcmp(tok, "TMPMBX")  == 0) mask |= PRV$M_TMPMBX;
        else if (strcmp(tok, "NETMBX")  == 0) mask |= PRV$M_NETMBX;
        else if (strcmp(tok, "BYPASS")  == 0) mask |= PRV$M_BYPASS;
        else if (strcmp(tok, "SYSLCK")  == 0) mask |= PRV$M_SYSLCK;
        else if (strcmp(tok, "SYSNAM")  == 0) mask |= PRV$M_SYSNAM;
        else if (strcmp(tok, "CMKRNL")  == 0) mask |= PRV$M_CMKRNL;
        else if (strcmp(tok, "ALL")     == 0) mask  = ~(uint64_t)0;

        tok = strtok_r(NULL, ",", &saveptr);
    }
    return mask;
}

/*
 * Parse a single sysuaf.dat line into a uaf_record.
 * Returns 1 on success, 0 if line is a comment or blank, -1 on parse error.
 */
static int parse_uaf_line(const char *line, struct uaf_record *rec)
{
    /* Skip comments and blank lines */
    if (!line || line[0] == '#' || line[0] == '\n' || line[0] == '\0')
        return 0;

    /* Copy line for strtok (it modifies in place) */
    char buf[SYSUAF_LINE_MAX];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Strip trailing newline */
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n')
        buf[len - 1] = '\0';

    memset(rec, 0, sizeof(*rec));

    char *f;
    char *saveptr = NULL;

    /* Fields separated by '|' (pipe) to avoid conflict with VMS device colons.
     * Format: USERNAME|PASSWORD_HASH|UIC_GROUP|UIC_MEMBER|DEFAULT_DIR|FLAGS|PRIVILEGES */
    f = strtok_r(buf, "|", &saveptr);
    if (!f) return -1;
    strncpy(rec->username, f, sizeof(rec->username) - 1);

    f = strtok_r(NULL, "|", &saveptr);
    if (!f) return -1;
    strncpy(rec->password_hash, f, sizeof(rec->password_hash) - 1);

    f = strtok_r(NULL, "|", &saveptr);
    if (!f) return -1;
    rec->uic_group = (unsigned int)strtoul(f, NULL, 10);

    f = strtok_r(NULL, "|", &saveptr);
    if (!f) return -1;
    rec->uic_member = (unsigned int)strtoul(f, NULL, 10);

    f = strtok_r(NULL, "|", &saveptr);
    if (!f) return -1;
    strncpy(rec->default_dir, f, sizeof(rec->default_dir) - 1);

    f = strtok_r(NULL, "|", &saveptr);
    if (f)
        rec->flags = (uint32_t)strtoul(f, NULL, 0);

    f = strtok_r(NULL, "|\n", &saveptr);
    if (f)
        strncpy(rec->privileges, f, sizeof(rec->privileges) - 1);

    return 1;
}

/*
 * Find a user record in sysuaf.dat by username.
 * username_str is a null-terminated C string (already extracted from descriptor).
 * Returns 1 if found, 0 if not found, -1 on I/O error.
 */
static int find_uaf_record(const char *username_str,
                            struct uaf_record *out_rec,
                            long *out_offset)
{
    char sysuaf_linux[1024];
    vmsfs_to_linux_path(SYSUAF_PATH, sysuaf_linux, sizeof(sysuaf_linux));
    FILE *f = fopen(sysuaf_linux, "r");
    if (!f)
        return -1;

    char line[SYSUAF_LINE_MAX];
    long offset = 0;
    int found = 0;

    while (fgets(line, sizeof(line), f)) {
        struct uaf_record rec;
        int rc = parse_uaf_line(line, &rec);
        if (rc == 1) {
            if (strcasecmp(rec.username, username_str) == 0) {
                if (out_rec)    *out_rec    = rec;
                if (out_offset) *out_offset = offset;
                found = 1;
                break;
            }
        }
        offset = ftell(f);
    }

    fclose(f);
    return found;
}

/*
 * Fill a single item list entry from a uaf_record.
 * Returns SS$_NORMAL or an error code.
 */
static uint32_t fill_uai_item(const struct item_list_3 *item,
                               const struct uaf_record *rec)
{
    switch (item->item_code) {

        case UAI$_USERNAME: {
            uint16_t len = (uint16_t)strlen(rec->username);
            if (len > item->buflen) len = item->buflen;
            if (item->bufaddr) memcpy(item->bufaddr, rec->username, len);
            if (item->retlen) *item->retlen = len;
            break;
        }

        case UAI$_UIC: {
            if (item->bufaddr && item->buflen >= sizeof(uint32_t)) {
                uint32_t uic = ((uint32_t)rec->uic_group << 16) |
                               (uint32_t)rec->uic_member;
                *(uint32_t *)item->bufaddr = uic;
            }
            if (item->retlen) *item->retlen = sizeof(uint32_t);
            break;
        }

        case UAI$_DEFDEV: {
            /* Default device — for OVMX we return empty (no VMS device notation) */
            if (item->bufaddr && item->buflen > 0)
                ((char *)item->bufaddr)[0] = '\0';
            if (item->retlen) *item->retlen = 0;
            break;
        }

        case UAI$_DEFDIR: {
            uint16_t len = (uint16_t)strlen(rec->default_dir);
            if (len > item->buflen) len = item->buflen;
            if (item->bufaddr) memcpy(item->bufaddr, rec->default_dir, len);
            if (item->retlen) *item->retlen = len;
            break;
        }

        case UAI$_PRIV: {
            if (item->bufaddr && item->buflen >= sizeof(uint64_t)) {
                uint64_t mask = parse_privileges(rec->privileges);
                *(uint64_t *)item->bufaddr = mask;
            }
            if (item->retlen) *item->retlen = sizeof(uint64_t);
            break;
        }

        case UAI$_DEF_PRIV: {
            if (item->bufaddr && item->buflen >= sizeof(uint64_t)) {
                uint64_t mask = parse_privileges(rec->privileges);
                *(uint64_t *)item->bufaddr = mask;
            }
            if (item->retlen) *item->retlen = sizeof(uint64_t);
            break;
        }

        case UAI$_FLAGS: {
            if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                *(uint32_t *)item->bufaddr = rec->flags;
            if (item->retlen) *item->retlen = sizeof(uint32_t);
            break;
        }

        case UAI$_PWD: {
            /* Return raw hash bytes (up to 8 bytes / 64-bit quadword) */
            if (item->bufaddr && item->buflen >= 8) {
                memset(item->bufaddr, 0, 8);
                /* Copy hex string as raw bytes if present */
                const char *hash = rec->password_hash;
                size_t hlen = strlen(hash);
                size_t copy = (hlen < 16) ? hlen : 16;  /* up to 8 bytes */
                size_t i;
                for (i = 0; i + 1 < copy && i / 2 < 8; i += 2) {
                    char byte_str[3] = { hash[i], hash[i+1], '\0' };
                    ((uint8_t *)item->bufaddr)[i / 2] =
                        (uint8_t)strtoul(byte_str, NULL, 16);
                }
            }
            if (item->retlen) *item->retlen = 8;
            break;
        }

        case UAI$_LSTLOGIN_I:
        case UAI$_LSTLOGIN_N: {
            /* Not tracked in sysuaf.dat — return zero (epoch) */
            if (item->bufaddr && item->buflen >= sizeof(uint64_t))
                *(uint64_t *)item->bufaddr = 0;
            if (item->retlen) *item->retlen = sizeof(uint64_t);
            break;
        }

        default:
            /* Unknown item — skip silently (VMS behavior) */
            break;
    }
    return SS$_NORMAL;
}

/*
 * sys$getuai - Get User Authorization Information.
 *
 * Looks up the named user in /etc/ovmx/sysuaf.dat and fills in
 * the requested UAI$_ items.
 *
 * @param efn      Event flag (ignored — synchronous)
 * @param context  Context longword for iterating (ignored)
 * @param usrnam   Descriptor of username to look up
 * @param itmlst   Item list of UAI$_ codes to retrieve
 * @param iosb     Optional I/O status block
 * @param astadr   AST completion routine (ignored)
 * @param astprm   AST parameter (ignored)
 */
uint32_t sys$getuai(uint32_t efn, uint32_t *context,
                    struct dsc$descriptor_s *usrnam,
                    void *itmlst, struct _iosb *iosb,
                    void (*astadr)(uint32_t), uint32_t astprm)
{
    (void)efn; (void)context; (void)astadr; (void)astprm;

    if (!usrnam || !usrnam->dsc$a_pointer)
        return SS$_BADPARAM;
    if (!itmlst)
        return SS$_BADPARAM;

    /* Extract username as C string */
    char username[32];
    dsc$strncpy(username, usrnam, sizeof(username));

    /* Look up the user record */
    struct uaf_record rec;
    int found = find_uaf_record(username, &rec, NULL);

    uint32_t status;
    if (found <= 0) {
        status = SS$_NOSUCHID;
        if (iosb) {
            iosb->iosb$w_status = (uint16_t)status;
            iosb->iosb$w_bcnt   = 0;
        }
        return status;
    }

    /* Walk the item list and fill each requested item */
    const struct item_list_3 *items = (const struct item_list_3 *)itmlst;
    for (; items->buflen != 0 || items->item_code != 0; items++) {
        fill_uai_item(items, &rec);
    }

    status = SS$_NORMAL;
    if (iosb) {
        iosb->iosb$w_status = (uint16_t)status;
        iosb->iosb$w_bcnt   = 0;
    }
    return status;
}

/*
 * sys$setuai - Set User Authorization Information.
 *
 * Updates a user's record in /etc/ovmx/sysuaf.dat.
 * Requires SYSPRV privilege.
 *
 * Implementation: reads all lines, rewrites the file with the
 * modified line in place.
 */
uint32_t sys$setuai(uint32_t efn, uint32_t *context,
                    struct dsc$descriptor_s *usrnam,
                    void *itmlst, struct _iosb *iosb,
                    void (*astadr)(uint32_t), uint32_t astprm)
{
    (void)efn; (void)context; (void)astadr; (void)astprm;

    if (!usrnam || !usrnam->dsc$a_pointer)
        return SS$_BADPARAM;

    /* Check for SYSPRV privilege */
    struct vms_pcb *pcb = vms_pcb_get();
    if (pcb && !(pcb->cur_privs & PRV$M_SYSPRV)) {
        return SS$_NOPRIV;
    }

    char username[32];
    dsc$strncpy(username, usrnam, sizeof(username));

    /* Read current record */
    struct uaf_record rec;
    int found = find_uaf_record(username, &rec, NULL);
    if (found <= 0) {
        return SS$_NOSUCHID;
    }

    /* Apply item list updates to the in-memory record */
    if (itmlst) {
        const struct item_list_3 *items = (const struct item_list_3 *)itmlst;
        for (; items->buflen != 0 || items->item_code != 0; items++) {
            switch (items->item_code) {
                case UAI$_FLAGS:
                    if (items->bufaddr && items->buflen >= sizeof(uint32_t))
                        rec.flags = *(uint32_t *)items->bufaddr;
                    break;
                case UAI$_DEFDIR:
                    if (items->bufaddr) {
                        size_t len = items->buflen;
                        if (len >= sizeof(rec.default_dir))
                            len = sizeof(rec.default_dir) - 1;
                        memcpy(rec.default_dir, items->bufaddr, len);
                        rec.default_dir[len] = '\0';
                    }
                    break;
                case UAI$_PWD:
                    /* Update password hash — encode bytes back to hex */
                    if (items->bufaddr && items->buflen >= 8) {
                        const uint8_t *bytes = (const uint8_t *)items->bufaddr;
                        snprintf(rec.password_hash, sizeof(rec.password_hash),
                                 "%02x%02x%02x%02x%02x%02x%02x%02x",
                                 bytes[0], bytes[1], bytes[2], bytes[3],
                                 bytes[4], bytes[5], bytes[6], bytes[7]);
                    }
                    break;
                default:
                    break;
            }
        }
    }

    /* Rewrite sysuaf.dat with the updated record */
    char sysuaf_linux2[1024];
    vmsfs_to_linux_path(SYSUAF_PATH, sysuaf_linux2, sizeof(sysuaf_linux2));
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.TMP", sysuaf_linux2);
    FILE *in  = fopen(sysuaf_linux2, "r");
    FILE *out = fopen(tmp_path, "w");
    if (!in || !out) {
        if (in)  fclose(in);
        if (out) fclose(out);
        return SS$_FILACCERR;
    }

    char line[SYSUAF_LINE_MAX];
    while (fgets(line, sizeof(line), in)) {
        struct uaf_record tmp;
        int rc = parse_uaf_line(line, &tmp);
        if (rc == 1 && strcasecmp(tmp.username, username) == 0) {
            /* Write the updated record */
            fprintf(out, "%s|%s|%u|%u|%s|%u|%s\n",
                    rec.username, rec.password_hash,
                    rec.uic_group, rec.uic_member,
                    rec.default_dir, rec.flags, rec.privileges);
        } else {
            fputs(line, out);
        }
    }

    fclose(in);
    fclose(out);
    rename(tmp_path, sysuaf_linux2);

    uint32_t status = SS$_NORMAL;
    if (iosb) {
        iosb->iosb$w_status = (uint16_t)status;
        iosb->iosb$w_bcnt   = 0;
    }
    return status;
}
