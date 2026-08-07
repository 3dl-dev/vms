/*
 * sys_uai.c - User Authorization File System Services
 *
 * Implements sys$getuai and sys$setuai for querying and updating
 * user account information from SYS$SYSTEM:SYSUAF.DAT.
 *
 * The format is PIPE-delimited text (the delimiter parse_uaf_line() below
 * splits on, and the one src/libvms/rtl/sysuaf.c writes):
 *   USERNAME|PASSWORD_HASH|UIC_GROUP|UIC_MEMBER|DEFAULT_DIR|FLAGS|PRIVILEGES
 *
 * sys$getuai fills in requested UAI$_ items from the matching record.
 * sys$setuai updates the record in place (requires SYSPRV privilege).
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * OVMX-USERSPACE: sys$getuai (vms-846.3) -- opens and parses the SYSUAF file
 *     itself through vmsfs path translation, in the calling process, with no
 *     executive-mediated access and no interlock against a concurrent
 *     sys$setuai in another process.
 * OVMX-PARTIAL: sys$setuai (vms-846.3) -- exec: the SYSPRV test reads the
 *     privilege mask the EXECUTIVE holds for the caller, through
 *     vms_kif_getjpi_self(). That mask arrives only through
 *     VMS_IOCTL_SETIDENT, which refuses any caller without SETPRV an identity
 *     that is not a weakening of its own (src/kernel/vms_proctab.c). This line
 *     is an upgrade rather than a correction -- see the disposition on
 *     sys$setuai below (vms-cb5).
 * OVMX-LOCAL: sys$setuai -- everything else. Once the test passes, this
 *     process rewrites SYSUAF.DAT itself through vmsfs path translation, with
 *     no executive mediation of the file and no interlock against a concurrent
 *     sys$getuai in another process.
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
#include "vms_kif.h"

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

    /*
     * FIELDS ARE SPLIT ON EVERY '|', INCLUDING CONSECUTIVE ONES (vms-cb5
     * round 5).
     *
     * This used to be seven strtok_r(buf, "|") calls. strtok treats a RUN of
     * delimiters as ONE, so every EMPTY field in a SYSUAF row was silently
     * dropped and every field after it read one position early. Five of the
     * six rows OVMX ships have an empty field, so this misparsed nearly the
     * whole authorization database:
     *
     *     USER1||200|202|SYS$SYSDEVICE:[USERS.USER1]||TMPMBX,NETMBX
     *
     * parsed as password_hash="200", uic_group=202 (octal 130), uic_member=
     * strtoul("SYS$SYSDEVICE:[USERS.USER1]", 8) = 0. MEASURED on the real
     * runtime by tests/qemu/test_syssvc_setuai.c, which read the row back out
     * of the file after a $SETUAI and found 202 and 0 where 200 and 202
     * belong. $GETUAI answered from the same misparse, so it reported the
     * wrong hash, the wrong UIC and the wrong privileges for those accounts;
     * $SETUAI then wrote the misparse back, which is how a service that
     * manages the authorization database silently rewrote an account's
     * identity.
     *
     * UIC MEMBER 0 IS WHY THIS IS NOT MERELY A PARSING BUG. tools/vms_login.c
     * does setuid(rec->uic_member) to drop the session's Linux credentials,
     * and setuid(0) is not a drop -- such a session keeps CAP_SYS_ADMIN, and
     * every process it creates registers with SETPRV. What stops that on the
     * shipped SYSUAF is that all four accounts this misparse gives member 0
     * carry no password hash and so cannot authenticate at all (vms-08f); it
     * is not stopped by anything here.
     *
     * The split below is the one src/libvms/rtl/sysuaf.c's sysuaf_scan()
     * already uses -- strchr for the next '|', NUL it, carry on -- so the two
     * readers of this file agree by construction rather than by coincidence.
     */
    char *fields[7];
    int nf;
    {
        char *p = buf;
        for (nf = 0; nf < 7 && p; nf++) {
            fields[nf] = p;
            char *sep = strchr(p, '|');
            if (sep) {
                *sep = '\0';
                p = sep + 1;
            } else {
                p = NULL;
            }
        }
    }

    /* USERNAME|PASSWORD_HASH|UIC_GROUP|UIC_MEMBER|DEFAULT_DIR|FLAGS|PRIVILEGES
     * -- the first five are required; a row with fewer is malformed. */
    if (nf < 5)
        return -1;

    strncpy(rec->username, fields[0], sizeof(rec->username) - 1);
    strncpy(rec->password_hash, fields[1], sizeof(rec->password_hash) - 1);
    /* OCTAL: SYSUAF.DAT UIC fields (vms-e60; derivation in
     * src/libvms/rtl/sysuaf.c). */
    rec->uic_group  = (unsigned int)strtoul(fields[2], NULL, 8);
    rec->uic_member = (unsigned int)strtoul(fields[3], NULL, 8);
    strncpy(rec->default_dir, fields[4], sizeof(rec->default_dir) - 1);
    if (nf > 5)
        rec->flags = (uint32_t)strtoul(fields[5], NULL, 0);
    if (nf > 6)
        strncpy(rec->privileges, fields[6], sizeof(rec->privileges) - 1);

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

    /*
     * THE SYSPRV TEST READS THE EXECUTIVE (vms-cb5 round 5).
     *
     * WHAT STOOD HERE:
     *
     *     struct vms_pcb *pcb = vms_pcb_get();
     *     if (pcb && !(pcb->cur_privs & PRV$M_SYSPRV))
     *         return SS$_NOPRIV;
     *
     * Two defects, and the second is the one that decides. The mask came
     * from the caller's own PCB, a per-process word sys$setprv
     * (src/libvms/syssvc/sys_misc.c) lets any process write for itself --
     * so the authorization was the caller's own claim. AND the guard read
     * `pcb &&`: vms_pcb_get() returns NULL for a process that never called
     * vms_pcb_init() (src/vmsprocess/vms_pcb.c), so for such a caller the
     * condition was false and the test was SKIPPED. Not a test the caller
     * controls -- no test. What is behind it is the UAI$_PWD case below,
     * which writes an account's password hash into SYSUAF.DAT.
     *
     * The mask now comes from the row the EXECUTIVE holds for this process,
     * the same source tools/vms_authorize.c's check_privilege() reads
     * (vms-b2e). A process cannot widen that row for itself: the authorized
     * mask is derived at registration from capable(CAP_SYS_ADMIN), and the
     * only way to change it is VMS_IOCTL_SETIDENT, which without SETPRV
     * accepts nothing but a weakening (src/kernel/vms_proctab.c).
     *
     * THERE IS NO ABSENT-EXECUTIVE BRANCH AND MUST NOT BE ONE (CLAUDE.md
     * Rule 9). PID 1 refuses to bring OVMX up without /dev/vms, so on the
     * one OVMX runtime this read cannot fail. If it fails anyway, this
     * service refuses: a process whose privileges nothing can vouch for
     * holds none. Substituting a process-local guess for a failed executive
     * read is the defect above wearing a different name.
     *
     * PRV$M_SYSPRV is held equal to the executive's own VMS_PRV_M_SYSPRV by
     * _Static_assert in src/libvms/prv_agreement.c, so the bit is pinned and
     * not self-certified (CLAUDE.md Rule 8).
     */
    {
        struct vms_procinfo self;

        memset(&self, 0, sizeof(self));
        if (!(vms_kif_getjpi_self(&self) & 1))
            return SS$_NOPRIV;
        if (!(self.cur_privs & PRV$M_SYSPRV))
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
            /*
             * %o, NOT %u, ON THE TWO UIC FIELDS (vms-e60, vms-cb5 round 5).
             *
             * parse_uaf_line() above reads them with strtoul(..., 8) -- the
             * base vms-e60 derived from the oracle and moved every other
             * SYSUAF reader and writer to. This fprintf was the site that
             * did not move: it read octal and wrote decimal, so rewriting
             * ANY record whose UIC digits differ between the two bases
             * changed that account's UIC. DEFAULT ships 200|200, which this
             * parsed as 128/128 and wrote back as "128|128", which the next
             * read takes as octal 88/88 -- a different UIC, hence a
             * different Linux uid/gid for the session LOGINOUT starts and a
             * different owner for every protection decision taken against
             * it. SYSTEM's 1|4 is the only shipped row that reads the same
             * in both bases, which is what hid this.
             *
             * tools/vms_authorize.c's writer already uses %o; this is the
             * same value written the same way, not a second answer.
             */
            fprintf(out, "%s|%s|%o|%o|%s|%u|%s\n",
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
