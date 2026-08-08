/*
 * sys_uai.c - User Authorization File System Services
 *
 * Implements sys$getuai and sys$setuai for querying and updating
 * user account information from SYS$SYSTEM:SYSUAF.DAT.
 *
 * The format is PIPE-delimited text, defined once in src/libvms/include/
 * sysuaf.h and parsed once in src/libvms/rtl/sysuaf.c:
 *   USERNAME|PASSWORD_HASH|UIC_GROUP|UIC_MEMBER|DEFAULT_DIR|FLAGS|PRIVILEGES
 *
 * sys$getuai fills in requested UAI$_ items from the matching record.
 * sys$setuai updates the record in place (requires SYSPRV privilege).
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * OVMX-PARTIAL: sys$getuai (vms-96e2) -- exec: the SYSUAF filespec is resolved
 *     to a Linux path through the executive-resident LNM$SYSTEM table (vmsfs
 *     path translation -> lnm_translate -> vms_kif_lnm_translate) for system
 *     logical names.
 * OVMX-LOCAL: sys$getuai -- opens and parses the SYSUAF file itself in the
 *     calling process, with no executive-mediated access and no interlock
 *     against a concurrent sys$setuai in another process.
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
#include <unistd.h>
#include "starlet.h"
#include "uaidef.h"
#include "prvdef.h"
#include "vms_kif.h"

/* Path to the system authorization file */
#include "ovmx_layout.h"
#include "vmsfs/filespec.h"
/*
 * THE FORMAT, THE READER AND THE WRITER ALL COME FROM libvms (vms-9b7).
 *
 * WHAT USED TO STAND HERE, so it is never put back:
 *   - a private "#define SYSUAF_LINE_MAX 512", one of THREE different line
 *     limits for one file format across five parsers (512 here and in PID 1,
 *     1024 in rtl/sysuaf.c and in AUTHORIZE). A row between those sizes was
 *     written happily by one and truncated silently by another;
 *   - a private record struct whose FLAGS field was a longword while
 *     every other reader of the same field held a string;
 *   - a private parse_uaf_line() and a private parse_privileges();
 *   - a second fprintf format string that wrote FLAGS "%u" where
 *     tools/vms_authorize.c wrote it "%s" -- the same field of the same file
 *     with two representations, so AUTHORIZE round-tripped an empty FLAGS as
 *     empty and $SETUAI rewrote it as "0".
 *
 * ALSO DELETED, and named here so the claim is not quietly reintroduced: a
 * comment asserting that PID 1 "must stay statically linked against the
 * minimal set of libraries ... it does not link libvms", offered as the
 * justification for duplicating this parser. PID 1 links FOUR libraries; the
 * constraint on it is STATIC, not minimal. And as of vms-9b7 PID 1 does not
 * read SYSUAF at all, so the justification has no subject left.
 */
#include "sysuaf.h"

/*
 * Find a user record in sysuaf.dat by username.
 * username_str is a null-terminated C string (already extracted from descriptor).
 * Returns 1 if found, 0 if not found, -1 on I/O error.
 */
static int find_uaf_record(const char *username_str,
                            sysuaf_record_t *out_rec,
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
    int too_long = 0;

    while (sysuaf_read_line(f, line, sizeof(line), &too_long)) {
        sysuaf_record_t rec;
        /* An over-length line is not a short record. Reporting and skipping
         * it is the whole fix (vms-9b7): parsing its prefix is how a
         * seven-field row became a five-field one. */
        if (too_long) {
            offset = ftell(f);
            continue;
        }
        int rc = sysuaf_parse_line(line, &rec);
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
                               const sysuaf_record_t *rec)
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
                uint64_t mask = sysuaf_parse_privileges(rec->privileges);
                *(uint64_t *)item->bufaddr = mask;
            }
            if (item->retlen) *item->retlen = sizeof(uint64_t);
            break;
        }

        case UAI$_DEF_PRIV: {
            if (item->bufaddr && item->buflen >= sizeof(uint64_t)) {
                uint64_t mask = sysuaf_parse_privileges(rec->privileges);
                *(uint64_t *)item->bufaddr = mask;
            }
            if (item->retlen) *item->retlen = sizeof(uint64_t);
            break;
        }

        case UAI$_FLAGS: {
            if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                *(uint32_t *)item->bufaddr =
                    sysuaf_flags_to_mask(rec->flags);
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
    sysuaf_record_t rec;
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
    sysuaf_record_t rec;
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
                        sysuaf_mask_to_flags(*(uint32_t *)items->bufaddr,
                                             rec.flags, sizeof(rec.flags));
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

    /*
     * REWRITE THROUGH THE ONE WRITER (vms-9b7).
     *
     * The line this loop used to emit came from a SECOND fprintf format
     * string that disagreed with tools/vms_authorize.c's on the FLAGS field
     * ("%u" here, "%s" there). It now calls sysuaf_format_record(), which is
     * the only place in the tree that turns a record into a line -- and which
     * REFUSES a record too long to be read back rather than writing one that
     * a reader will silently truncate.
     *
     * Non-matching rows are still copied VERBATIM, including any that this
     * process could not parse: $SETUAI is asked to change ONE account, and a
     * row it does not understand is not its to rewrite or to drop.
     */
    char line[SYSUAF_LINE_MAX];
    int too_long_w = 0;
    while (sysuaf_read_line(in, line, sizeof(line), &too_long_w)) {
        if (too_long_w) {
            /*
             * An over-length row cannot be copied verbatim (this process only
             * ever saw its prefix) and cannot be parsed. Copying the prefix
             * would DESTROY that account; dropping it would delete it. So the
             * rewrite is abandoned and the original file is left untouched --
             * the only answer that loses nothing.
             */
            fclose(in);
            fclose(out);
            unlink(tmp_path);
            if (iosb) {
                iosb->iosb$w_status = (uint16_t)SS$_FILACCERR;
                iosb->iosb$w_bcnt   = 0;
            }
            return SS$_FILACCERR;
        }

        /* sysuaf_parse_line() modifies its argument, so the verbatim copy
         * has to be taken BEFORE the parse, not after it. */
        char verbatim[SYSUAF_LINE_MAX];
        strncpy(verbatim, line, sizeof(verbatim) - 1);
        verbatim[sizeof(verbatim) - 1] = '\0';

        sysuaf_record_t tmp;
        int rc = sysuaf_parse_line(line, &tmp);
        if (rc == 1 && strcasecmp(tmp.username, username) == 0) {
            char row[SYSUAF_LINE_MAX];
            if (sysuaf_format_record(&rec, row, sizeof(row)) < 0) {
                /* Loud writer-side refusal. The update does not happen and
                 * the caller is told, rather than a truncated identity being
                 * committed to the authorization database. */
                fclose(in);
                fclose(out);
                unlink(tmp_path);
                if (iosb) {
                    iosb->iosb$w_status = (uint16_t)SS$_BADPARAM;
                    iosb->iosb$w_bcnt   = 0;
                }
                return SS$_BADPARAM;
            }
            fprintf(out, "%s\n", row);
        } else {
            fputs(verbatim, out);
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
