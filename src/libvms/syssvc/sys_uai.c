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
 * Find a user record in the binary SYSUAF by username (vms-d92 atomic flip).
 * $GETUAI/$SETUAI now read the genuine $UAFDEF record through the binary engine
 * (sysuaf_lookup -> ovmx_sysuaf_read_user over the ACP, or the POSIX defer when
 * /dev/vms is absent) -- no ASCII parse, no SHA-256. Fail-honest: a missing
 * account or an image without LIBVMSRMS returns 0/-1, never a fabricated record.
 * Returns 1 if found, 0 if not found.
 */
static int find_uaf_record(const char *username_str, sysuaf_record_t *out_rec)
{
    sysuaf_record_t rec;
    if (sysuaf_lookup(username_str, &rec) != 0)
        return 0;
    if (out_rec)
        *out_rec = rec;
    return 1;
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
            /* The REAL Purdy password quadword (uaf$q_pwd), 8 bytes verbatim
             * from the $UAFDEF record -- not decoded from a hex string. */
            if (item->bufaddr && item->buflen >= 8)
                memcpy(item->bufaddr, rec->raw.uaf$q_pwd, 8);
            if (item->retlen) *item->retlen = 8;
            break;
        }

        case UAI$_SALT: {
            /* The real per-account salt word (uaf$w_salt), 2 bytes. */
            if (item->bufaddr && item->buflen >= 2)
                memcpy(item->bufaddr, rec->raw.uaf$w_salt, 2);
            if (item->retlen) *item->retlen = 2;
            break;
        }

        case UAI$_ENCRYPT: {
            /* The algorithm byte (uaf$b_encrypt; UAI$C_PURDY_S == 3). */
            if (item->bufaddr && item->buflen >= 1)
                *(uint8_t *)item->bufaddr = rec->raw.uaf$b_encrypt;
            if (item->retlen) *item->retlen = 1;
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
    int found = find_uaf_record(username, &rec);

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

    /* Read current record (binary $UAFDEF via the ACP; vms-d92). */
    sysuaf_record_t rec;
    int found = find_uaf_record(username, &rec);
    if (found <= 0) {
        return SS$_NOSUCHID;
    }

    /*
     * Apply item-list updates to the in-memory record, then write the BINARY
     * record back in place via the one binary writer (sysuaf_write_record ->
     * ovmx_sysuaf_store_user -> $UPDATE over the ACP). No ASCII, no fopen, no
     * whole-file rewrite: the username and UIC keys are unchanged, so the
     * update is a same-length in-place record modify (Rule 9 / INV-6).
     */
    int wrote_pwd = 0;
    uint8_t new_pwd[8];
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
                    /* The caller supplies the already-hashed quadword; store
                     * those 8 bytes verbatim into uaf$q_pwd (matching VMS,
                     * which writes the hashed value $SETUAI is handed). */
                    if (items->bufaddr && items->buflen >= 8) {
                        memcpy(new_pwd, items->bufaddr, 8);
                        wrote_pwd = 1;
                    }
                    break;
                default:
                    break;
            }
        }
    }

    /* Fold the text edits (FLAGS/DEFDIR) into the binary record; this preserves
     * the password area, so a UAI$_PWD written below survives. */
    sysuaf_view_to_raw(&rec);
    if (wrote_pwd)
        memcpy(rec.raw.uaf$q_pwd, new_pwd, 8);

    if (sysuaf_write_record(&rec) != 0) {
        if (iosb) {
            iosb->iosb$w_status = (uint16_t)SS$_FILACCERR;
            iosb->iosb$w_bcnt   = 0;
        }
        return SS$_FILACCERR;
    }

    uint32_t status = SS$_NORMAL;
    if (iosb) {
        iosb->iosb$w_status = (uint16_t)status;
        iosb->iosb$w_bcnt   = 0;
    }
    return status;
}
