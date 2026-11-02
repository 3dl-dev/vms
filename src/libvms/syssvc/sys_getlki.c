/*
 * sys_getlki.c - SYS$GETLKI / SYS$GETLKIW (rd vms-b71), split out of
 * sys_lock.c (rd vms-c06d) so the service register can see that this
 * translation unit, and only this one, holds $GETLKI's userspace half: the
 * counted LKI$_RESNAM copy into the caller's buffer.
 *
 * OVMX service register (rd vms-d89) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * OVMX-PARTIAL: sys$getlki (vms-b71) -- exec: every
 *     item getlki_impl answers (LKI$_LOCKID/RESNAM/GRANTCOUNT/PARENT/STATE/PID) comes
 *     from the kernel lock manager's real lock+resource state
 *     (vms_kif_getlki_parent -> VMS_IOCTL_GETLKI -> vms_ioctl_getlki,
 *     kernel-core/vms_lock.c), including GRANTCOUNT's resource->granted walk. The
 *     item-list loop here only VALIDATES codes (SS$_BADPARAM for one this
 *     executive cannot answer) and reformats the SAME executive-supplied
 *     longwords/text/bytes into the caller's buffer -- no value or decision is
 *     computed in this file, the same relabeling-is-not-a-substitute distinction
 *     $ENQ/$DEQ's kstat_to_ss removal rests on above.
 * OVMX-LOCAL: sys$getlki -- copying LKI$_RESNAM into the caller's buffer as the
 *     COUNTED name the executive reports (resnam_len bytes, NULs included,
 *     truncated to the buffer; rd vms-c06d): the getlki-resnam-cstring negative
 *     control proves it with test_syssvc_getlki.
 * OVMX-PARTIAL: sys$getlkiw (vms-b71) -- exec: the
 *     same service as sys$getlki; getlki_impl runs synchronously already, so
 *     sys$getlkiw IS sys$getlki (see the two functions below).
 * OVMX-LOCAL: sys$getlkiw -- the same counted LKI$_RESNAM copy as sys$getlki.
 */
#include <stdint.h>
#include <string.h>
#include "starlet.h"
#include "lkidef.h"   /* LKI$_ item codes, LKI$C_ state values ($GETLKI) */
#include "lnmdef.h"   /* struct item_list_3 ($GETLKI's item list) */
#include "vms_kif.h"
#include "ovmx_async.h"  /* vms$$async_begin/finish ($GETLKI's completion) */
#include "ssdef.h"

/*
 * lki_item_defined - whether `code` is an LKI$_ item this executive can
 * answer from REAL lock state (INV-6: no fabricated item answers an
 * unsupported code's caller would otherwise read as success).
 *
 * LKI$_PID (rd vms-6f2) is in the set: the executive reads the owning
 * process's PID off the lock block (vms_ioctl_getlki's holder_pid). It was
 * once refused here outright, which made $GETLKI reject any list naming it --
 * RMHOLD's six-item read-back, among others. The one case the executive
 * cannot answer, a master copy this node holds FOR a remote node's process,
 * is refused per lock in getlki_impl (see there), never answered with a
 * made-up PID.
 *
 * Any other code fails the whole call with SS$_BADPARAM, as VMS does: the
 * OpenVMS System Services Reference Manual's $GETLKI "Condition Values
 * Returned" lists "SS$_BADPARAM  You specified an invalid item code."
 */
static int lki_item_defined(uint32_t code)
{
    switch (code) {
    case LKI$_STATE:
    case LKI$_PARENT:
    case LKI$_LOCKID:
    case LKI$_RESNAM:
    case LKI$_GRANTCOUNT:
    case LKI$_PID:
        return 1;
    default:
        return 0;
    }
}

/* A longword item: as many bytes as the buffer holds, that many returned
 * (the same SHORTBUF convention sys$getsyi's syi_put_long uses). */
static void lki_put_long(const struct item_list_3 *item, uint32_t v)
{
    uint16_t n = item->buflen < 4 ? item->buflen : 4;
    if (item->bufaddr && n)
        memcpy(item->bufaddr, &v, n);
    if (item->retlen) *item->retlen = n;
}

/* LKI$_RESNAM: the resource name, a COUNTED byte string (rd vms-c06d) --
 * `len` bytes, NULs included (RMS's file-access lock names hold binary FIDs),
 * truncated to the caller's buffer. */
static void lki_put_bytes(const struct item_list_3 *item, const uint8_t *s,
                          size_t len)
{
    uint16_t n = (uint16_t)(len > item->buflen ? item->buflen : len);
    if (item->bufaddr && n)
        memcpy(item->bufaddr, s, n);
    if (item->retlen) *item->retlen = n;
}

/*
 * lki_put_state - LKI$_STATE's real answer shape: the 3-byte STATEF
 * structure the OpenVMS VAX V7.3 / Alpha V8.4 STARLET.MLB oracle defines
 * (docs/oracle/{vax73,alpha84}-starlet-defs/LKIDEF.txt, LKI$S_STATEF == 3) --
 * byte 0 the requested mode, byte 1 the granted mode, byte 2 the queue-state
 * code. There is no separate top-level LKI$_GRMODE/LKI$_RQMODE item on real
 * VMS; this is where those two values actually live.
 *
 * The queue byte answers only LKI$C_GRANTED or LKI$C_CONVERT -- the binary
 * granted/waiting distinction the kernel's `lock->waiting` flag genuinely
 * tracks (vms_ioctl_getlki: requested_mode == granted_mode exactly when not
 * waiting). Real VMS's queue byte can also read WAITING/RETRY/SCSWAIT/
 * RSPxxx (cross-node negotiation sub-states, LKIDEF.txt); the executive does
 * not track those as distinct states yet, so this never claims one (INV-6).
 */
static void lki_put_state(const struct item_list_3 *item,
                           uint32_t requested_mode, uint32_t granted_mode)
{
    uint8_t statef[3];
    statef[LKI$B_STATE_RQMODE] = (uint8_t)requested_mode;
    statef[LKI$B_STATE_GRMODE] = (uint8_t)granted_mode;
    statef[LKI$B_STATE_QUEUE] = (granted_mode != requested_mode)
                                     ? LKI$C_CONVERT : LKI$C_GRANTED;

    uint16_t n = item->buflen < sizeof(statef) ? item->buflen : (uint16_t)sizeof(statef);
    if (item->bufaddr && n)
        memcpy(item->bufaddr, statef, n);
    if (item->retlen) *item->retlen = n;
}

/*
 * getlki_impl - the body of sys$getlki/sys$getlkiw.
 *
 * Reads ONE lock's genuine state from the kernel lock manager
 * (vms_kif_getlki_parent -> VMS_IOCTL_GETLKI) and projects the requested
 * LKI$_ items from it. An item code this executive cannot answer from real
 * lock state fails the WHOLE request with SS$_BADPARAM, checked before the
 * kernel is even asked -- the same order sys$getsyi's getsyi_impl uses for
 * its own undefined-item check (SYI.BADITEM, docs/oracle/semantics/info/).
 * An invalid lock ID (SS$_IVLOCKID) is returned as-is with no item filled,
 * matching the lock services' own "a refused request leaves nothing behind"
 * convention (do_enq, above).
 */
static uint32_t getlki_impl(const uint32_t *lkidadr,
                             const struct item_list_3 *itmlst)
{
    if (!lkidadr || !itmlst)
        return SS$_BADPARAM;

    for (const struct item_list_3 *it = itmlst; it->buflen || it->item_code; it++)
        if (!lki_item_defined(it->item_code))
            return SS$_BADPARAM;

    uint32_t granted_mode = 0, requested_mode = 0, grant_count = 0, parent_id = 0;
    char resnam[32];
    uint8_t valblk[16];

    uint32_t status = vms_kif_getlki_parent(*lkidadr, &granted_mode,
                                             &requested_mode, resnam, valblk,
                                             &parent_id, &grant_count);
    if (!(status & 1))
        return status;

    /* LKI$_PID (rd vms-6f2): the owning process's PID as the executive reads
     * it off the lock block. Asked only when an item wants it. A lock with no
     * local owner (a master copy held for a remote node's process) has a PID
     * the executive does not hold; rather than invent one (or report 0 as if
     * it were a real PID), the request is refused as an item this executive
     * cannot answer for THIS lock, before any item is written. */
    uint32_t holder_pid = 0, holder_local = 0;
    for (const struct item_list_3 *it = itmlst; it->buflen || it->item_code; it++) {
        if (it->item_code == LKI$_PID) {
            status = vms_kif_getlki_holder(*lkidadr, &holder_pid, &holder_local);
            if (!(status & 1))
                return status;
            if (!holder_local)
                return SS$_BADPARAM;
            break;
        }
    }

    /* The name as the resource block counts it (vms_kif_getlki_resnam): the
     * C-string copy above stops at the first NUL. Asked only when an item
     * wants it. */
    uint8_t name[32];
    uint32_t name_len = 0;
    for (const struct item_list_3 *it = itmlst; it->buflen || it->item_code; it++) {
        if (it->item_code == LKI$_RESNAM) {
            status = vms_kif_getlki_resnam(*lkidadr, name, &name_len);
            if (!(status & 1))
                return status;
            if (name_len > sizeof name)
                name_len = sizeof name;
            break;
        }
    }

    for (const struct item_list_3 *item = itmlst;
         item->buflen != 0 || item->item_code != 0; item++) {
        switch (item->item_code) {
            case LKI$_LOCKID:
                lki_put_long(item, *lkidadr);
                break;
            case LKI$_RESNAM:
                lki_put_bytes(item, name, name_len);
                break;
            case LKI$_GRANTCOUNT:
                lki_put_long(item, grant_count);
                break;
            case LKI$_PARENT:
                lki_put_long(item, parent_id);
                break;
            case LKI$_STATE:
                lki_put_state(item, requested_mode, granted_mode);
                break;
            case LKI$_PID:
                lki_put_long(item, holder_pid);
                break;
        }
    }

    return SS$_NORMAL;
}

/*
 * sys$getlki - Get lock information (vms-b71).
 *
 * See getlki_impl for the body; this wraps it in the same completion
 * contract every item-list service here owes its caller (IOSB written,
 * event flag set, AST queued -- vms$$async_begin/finish, as sys$getsyi
 * uses them).
 */
uint32_t (sys$getlki)(uint32_t efn, const uint32_t *lkidadr, void *itmlst,
                      void *iosb, void (*astadr)(uint32_t), uint32_t astprm,
                      void *nullarg) {
    (void)nullarg;

    uint32_t rq = vms$$async_begin(efn);
    if (rq != SS$_NORMAL)
        return rq;

    uint32_t st = getlki_impl(lkidadr, (const struct item_list_3 *)itmlst);

    return vms$$async_finish(efn, iosb, st, astadr, astprm);
}

/*
 * sys$getlkiw - Get lock information (synchronous wrapper).
 *
 * getlki_impl runs synchronously already (it reads the kernel lock manager
 * and returns at once), so $GETLKIW is the same call as $GETLKI -- the same
 * relationship sys$getsyiw has to sys$getsyi.
 */
uint32_t (sys$getlkiw)(uint32_t efn, const uint32_t *lkidadr, void *itmlst,
                       void *iosb, void (*astadr)(uint32_t), uint32_t astprm,
                       void *nullarg) {
    return sys$getlki(efn, lkidadr, itmlst, iosb, astadr, astprm, nullarg);
}
