/*
 * sys_device.c - Device Information System Services
 *
 * Implements sys$getdvi / sys$getdviw / sys$device_scan as READERS of the
 * EXECUTIVE'S device table (src/kernel/vms_devtab.c), reached through the
 * vms_kif layer over /dev/vms. What these services report about a device is
 * what every process on the node sees -- not a per-process idea of what a
 * device looks like.
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * WHAT THESE THREE USED TO BE, and why the shape mattered more than the code.
 * Until vms-dv1 these services answered from classify_device() plus
 * statvfs()/termios on the HOST and a compiled-in scan_devices[] table: a
 * per-process fiction that reported SS$_NORMAL for devices the executive had
 * never heard of and enumerated the same fixed list on every system whatever
 * was actually configured. DCL's SHOW DEVICE and F$DEVICE were converted to
 * read the executive by vms-fb9, but they had to reach vms_kif_* DIRECTLY
 * because these services still fabricated -- tests/libvms/test_lib_fb3.c
 * asserted their invented answers. vms-911 filed that finding; vms-dv1 closes
 * it by making the PUBLIC services readers of the same one table, so a program
 * that never touched DCL sees exactly what SHOW DEVICE sees.
 *
 * OVMX-PARTIAL: sys$getdvi (vms-dv1) -- exec: the device's identity (name,
 *     class, type), ownership (owner PID and UIC), allocation and reference/
 *     error/operation counts come from the EXECUTIVE device table via
 *     vms_kif_getdvi_devnam() (by name) or vms_kif_getdvi_chan() (by an
 *     assigned channel). A device the executive does not have is refused
 *     (SS$_NOSUCHDEV), not fabricated -- the upgrade from the old
 *     classify_device()+statvfs() host fake (vms-911).
 * OVMX-LOCAL: sys$getdvi -- the DVI$_ item-code marshalling into the caller's
 *     item list runs in this process, and the volume/geometry items
 *     (MAXBLOCK/FREEBLOCKS/VOLNAM/MOUNTCNT/...) answer 0 because the executive
 *     does not track volume state yet (a documented vms-dv1 remainder: volume
 *     state arrives with MOUNT).
 * OVMX-PARTIAL: sys$getdviw (vms-dv1) -- exec: tail-calls sys$getdvi, so the
 *     executive supplies exactly the same half.
 * OVMX-LOCAL: sys$getdviw -- inherits sys$getdvi's userspace remainder exactly.
 * OVMX-PARTIAL: sys$alloc (vms-44a) -- exec: the device is resolved in the
 *     executive's device table and the allocation CLAIM is the executive's
 *     (vms_kif_alloc), so a second process is refused SS$_DEVALLOC.
 * OVMX-LOCAL: sys$alloc -- the physical-name echo (phylen/phybuf) is the table
 *     row's name with the leading underscore VMS prints; acmode/flags are not
 *     consulted.
 * OVMX-PARTIAL: sys$dalloc (vms-44a) -- exec: the release is the executive's
 *     (vms_kif_dalloc; SS$_DEVNOTALLOC if this process does not hold it).
 * OVMX-LOCAL: sys$dalloc -- only the descriptor-to-string copy and the device
 *     name resolution happen here.
 * OVMX-PARTIAL: sys$device_scan (vms-dv1) -- exec: every device it yields is a
 *     row of the EXECUTIVE'S I/O database, enumerated through vms_kif_devscan();
 *     it can no longer report a device the executive does not have.
 * OVMX-LOCAL: sys$device_scan -- the wildcard (wildnam) and DVS$_DEVCLASS
 *     filtering that select which of those executive rows the caller sees are
 *     applied in this process.
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "ovmx_async.h"
#include "starlet.h"
#include "devdef.h"
#include "dvidef.h"
#include "dcdef.h"
#include "dvsdef.h"
#include "lnmdef.h"
#include <strings.h>
#include "vms_kif.h"        /* pulls in struct vms_devinfo, VMS_DEVNAM_SIZE */

/*
 * VMS standard block size. Device-independent (a VMS logical block is 512
 * bytes on every device class), so it is a constant, not per-device state
 * fabricated for a device the executive does not describe.
 */
#define VMS_BLOCK_SIZE  512

/*
 * DVI$_DEVCHAR bits (DEV$M_) that OVMX can derive from the executive's
 * device row TODAY. This is a deliberately small set: the executive models a
 * device's class, type, ownership and allocation, so the two device-state
 * bits below are honest; the rest of the DEV$M_ namespace (SHR, MNT, RCK, WCK,
 * SPL, ...) is not tracked by the executive yet and is therefore NOT invented
 * here. The disk/volume flags the old fake set from statvfs are gone with it.
 */
/* DEV$M_ALL / DEV$M_AVL come from devdef.h: the V7.3 values (ALL 0x800000, AVL
 * 0x40000). This file once carried private copies (0x8 / 0x20) that were really
 * DEV$M_DIR and DEV$M_SQD on a real system. */

/*
 * dev_unit - the unit number as a pure function of the physical name
 * (trailing decimal digits before the colon). Derived, not fabricated: e.g.
 * "OPA0:" -> 0, "DKA100:" -> 100. No digits -> 0.
 */
static uint32_t dev_unit(const char *devnam)
{
    const char *p = devnam;
    const char *last_digit_run = NULL;

    /* Find the last run of digits (the unit trails the controller). */
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            if (!last_digit_run)
                last_digit_run = p;
        } else {
            last_digit_run = NULL;
        }
        p++;
    }
    if (!last_digit_run)
        return 0;
    return (uint32_t)strtoul(last_digit_run, NULL, 10);
}

/*
 * Fill one DVI item from an executive device row. Only item codes the
 * executive genuinely describes are answered with data; the rest return a
 * longword 0 (or empty string), never a host-derived value.
 */
static void fill_dvi_item(const struct item_list_3 *item,
                          const struct vms_devinfo *info)
{
    switch (item->item_code) {

    case DVI$_DEVNAM:
    case DVI$_FULLDEVNAM:
    case DVI$_ALLDEVNAM: {
        /* Single-node system: the full name is the physical name. A cluster
         * node prefix ("node$") is a documented remainder (vms-dv1).
         * DVI$_DEVNAM is the PHYSICAL name, which VMS writes with its leading
         * underscore: "_NLA0:" (DVI.DEVNAM.NLA0, docs/oracle/semantics/info/;
         * rd vms-de3a). */
        char nm[VMS_DEVNAM_SIZE + 2];
        snprintf(nm, sizeof nm, "%s%s",
                 (item->item_code == DVI$_DEVNAM && info->devnam[0] != '_') ? "_" : "",
                 info->devnam);
        uint16_t len = (uint16_t)strlen(nm);
        if (len > item->buflen) len = item->buflen;
        if (item->bufaddr) memcpy(item->bufaddr, nm, len);
        if (item->retlen) *item->retlen = len;
        break;
    }

    case DVI$_UNIT:
        if (item->bufaddr && item->buflen >= sizeof(uint32_t))
            *(uint32_t *)item->bufaddr = dev_unit(info->devnam);
        if (item->retlen) *item->retlen = sizeof(uint32_t);
        break;

    case DVI$_DEVCLASS:
        if (item->bufaddr && item->buflen >= sizeof(uint32_t))
            *(uint32_t *)item->bufaddr = info->devclass;
        if (item->retlen) *item->retlen = sizeof(uint32_t);
        break;

    case DVI$_DEVTYPE:
        if (item->bufaddr && item->buflen >= sizeof(uint32_t))
            *(uint32_t *)item->bufaddr = info->devtype;
        if (item->retlen) *item->retlen = sizeof(uint32_t);
        break;

    case DVI$_DEVCHAR: {
        /* The DEV$M_* characteristics the driver gave the unit when it was
         * entered (rd vms-de3a); for a row without them, only the bits the
         * executive can ground (see DEV$M_ note above). */
        uint32_t chars = info->devchar_dev ? info->devchar_dev : DEV$M_AVL;
        if (info->allocated) chars |= DEV$M_ALL;
        if (item->bufaddr && item->buflen >= sizeof(uint32_t))
            *(uint32_t *)item->bufaddr = chars;
        if (item->retlen) *item->retlen = sizeof(uint32_t);
        break;
    }

    case DVI$_REFCNT:
        if (item->bufaddr && item->buflen >= sizeof(uint32_t))
            *(uint32_t *)item->bufaddr = info->refcnt;
        if (item->retlen) *item->retlen = sizeof(uint32_t);
        break;

    case DVI$_ERRCNT:
        if (item->bufaddr && item->buflen >= sizeof(uint32_t))
            *(uint32_t *)item->bufaddr = info->errcnt;
        if (item->retlen) *item->retlen = sizeof(uint32_t);
        break;

    case DVI$_OPCNT:
        if (item->bufaddr && item->buflen >= sizeof(uint32_t))
            *(uint32_t *)item->bufaddr = (uint32_t)info->opcnt;
        if (item->retlen) *item->retlen = sizeof(uint32_t);
        break;

    case DVI$_PID:
        if (item->bufaddr && item->buflen >= sizeof(uint32_t))
            *(uint32_t *)item->bufaddr = info->owner_pid;
        if (item->retlen) *item->retlen = sizeof(uint32_t);
        break;

    case DVI$_OWNUIC:
        if (item->bufaddr && item->buflen >= sizeof(uint32_t))
            *(uint32_t *)item->bufaddr = info->owner_uic;
        if (item->retlen) *item->retlen = sizeof(uint32_t);
        break;

    case DVI$_AVAILABLE:
        /* A device the executive lists is available; it models no offline
         * state yet, so "present" is the honest answer to "available". */
        if (item->bufaddr && item->buflen >= sizeof(uint32_t))
            *(uint32_t *)item->bufaddr = 1;
        if (item->retlen) *item->retlen = sizeof(uint32_t);
        break;

    case DVI$_BLOCKSIZE:
        if (item->bufaddr && item->buflen >= sizeof(uint32_t))
            *(uint32_t *)item->bufaddr = VMS_BLOCK_SIZE;
        if (item->retlen) *item->retlen = sizeof(uint32_t);
        break;

    /* booleans VMS derives from the unit's characteristics (rd vms-de3a) */
    case DVI$_MBX:
    case DVI$_TRM:
    case DVI$_SPL: {
        uint32_t bit = item->item_code == DVI$_MBX ? DEV$M_MBX :
                       item->item_code == DVI$_TRM ? DEV$M_TRM : DEV$M_SPL;
        uint32_t chars = info->devchar_dev;
        if (!chars && info->devclass == DC$_TERM)
            chars = DEV$M_TRM;          /* a terminal row with no DEV$ word yet */
        if (item->bufaddr && item->buflen >= sizeof(uint32_t))
            *(uint32_t *)item->bufaddr = (chars & bit) ? 1u : 0u;
        if (item->retlen) *item->retlen = sizeof(uint32_t);
        break;
    }

    case DVI$_DEVBUFSIZ:
        if (item->bufaddr && item->buflen >= sizeof(uint32_t))
            *(uint32_t *)item->bufaddr = info->devbufsiz ? info->devbufsiz
                                       : (info->devclass == DC$_TERM ? info->width : 0u);
        if (item->retlen) *item->retlen = sizeof(uint32_t);
        break;

    default:
        /*
         * Everything else -- volume geometry (MAXBLOCK/FREEBLOCKS/VOLNAM/
         * MOUNTCNT/CLUSTER/...) and the rest of the DEV$M_/boolean space --
         * is not tracked by the executive yet (volume state arrives with
         * MOUNT; a documented remainder on vms-dv1). Answer a longword 0
         * rather than a host-derived value: an honest "not known" beats the
         * statvfs() fiction this rewrite removed.
         */
        if (item->bufaddr && item->buflen > 0)
            memset(item->bufaddr, 0,
                   (item->buflen < sizeof(uint32_t)) ?
                   item->buflen : sizeof(uint32_t));
        if (item->retlen) *item->retlen = 0;
        break;
    }
}

/*
 * device_lookup_translated - resolve devnam against the executive's device
 * table, falling back to LNM$SYSTEM logical-name translation when a literal
 * lookup misses (vms-08c).
 *
 * Real $GETDVI accepts a logical name for devnam -- SYS$SYSDEVICE: is the
 * standard VMS idiom for "the system disk", and every corpus program that
 * reads its free space passes it literally (tests/corpus/tier1-examples/
 * lib_getdvi.c, an unmodified Eight-Cubed download -- provenance forbids
 * editing it to use a physical name instead). The executive's device table
 * only holds physical unit names (OPA0:, DKA0:, ...; src/kernel/vms_devtab.c),
 * so a literal lookup on a logical name legitimately misses first. This does
 * NOT invent a per-process answer: it re-asks the SAME LNM$SYSTEM arena every
 * process on the node reads (vms_kif_lnm_translate, the identical call
 * src/libvms/syssvc/sys_logical.c's sys$trnlnm already makes), and retries the
 * physical lookup with whatever it finds. A device the executive genuinely
 * does not have -- because neither a unit nor a logical name resolves it --
 * still comes back SS$_NOSUCHDEV, not fabricated.
 *
 * Bounded to a few hops (a logical can name another logical, e.g.
 * SYS$DISK -> SYS$SYSDEVICE -> DKA0:) the same way
 * src/vmsfs/vmsfs_translate.c's vmsfs_resolve_device_r guards against a
 * translation loop. An equivalence is retried AS-IS, colon included --
 * lnm_seed_system_locating's device-class values are already a bare
 * physical spec ("DKA0:", matching vms_devtab's own stored form exactly) --
 * so a directory-bearing equivalence (SYS$SYSROOT-style, "SYS$SYSDEVICE:
 * [SYS0.]") simply fails the next physical lookup and the next LNM lookup in
 * turn, degrading to the honest SS$_NOSUCHDEV below rather than guessing
 * where the device name ends.
 */
#define DEVICE_XLATE_MAX_DEPTH 8

extern uint32_t vms$$chan_exec_chan(uint16_t chan);
extern const char *vms$$chan_devnam(uint16_t chan);

static uint32_t device_lookup_translated(const char *devnam_in,
                                         struct vms_devinfo *info)
{
    char cur[VMS_DEVNAM_SIZE];
    int depth;

    strncpy(cur, devnam_in, sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = '\0';

    for (depth = 0; depth < DEVICE_XLATE_MAX_DEPTH; depth++) {
        uint32_t status = vms_kif_getdvi_devnam(cur, info);
        if (status != SS$_NOSUCHDEV)
            return status;   /* SS$_NORMAL, or a different failure -- done */

        /* Not a physical unit; try it as a SYSTEM-table logical name.
         * Names are keyed WITHOUT a trailing colon (lnm_seed_system_locating,
         * src/vmslnm/lnm_defaults.c), so strip one before looking it up. */
        char key[VMS_DEVNAM_SIZE];
        size_t klen = strlen(cur);
        if (klen > 0 && cur[klen - 1] == ':')
            klen--;
        if (klen == 0 || klen >= sizeof(key))
            return SS$_NOSUCHDEV;
        memcpy(key, cur, klen);
        key[klen] = '\0';

        /* TT is THIS process's terminal: the device the executive's process
         * row names (rd vms-b14e; observed DVI.TT / DVI.SYSCOMMAND, a DC$_TERM
         * row, docs/oracle/semantics/info/). Its process-table equivalence is
         * the substrate path the byte I/O uses, not a device name. */
        if (!strcasecmp(key, "TT")) {
            struct vms_procinfo me;
            memset(&me, 0, sizeof me);
            if ((vms_kif_getjpi_self(&me) & 1) && me.terminal[0]) {
                strncpy(cur, me.terminal, sizeof(cur) - 1);
                cur[sizeof(cur) - 1] = '\0';
                continue;
            }
            return SS$_NOSUCHDEV;
        }

        /* A logical name in the device-name search order (LNM$FILE_DEV:
         * process, job, group, system -- SYS$COMMAND is a process name), as
         * $GETDVI translates its device argument. */
        char equiv[256];   /* >= VMS_LNM_MAX_VALUE+1 (src/kernel/vms_lnm.h) */
        {
            static const char fd[] = "LNM$FILE_DEV";
            struct dsc$descriptor_s td = { sizeof fd - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)fd };
            struct dsc$descriptor_s nd = { (uint16_t)strlen(key), DSC$K_DTYPE_T, DSC$K_CLASS_S, key };
            uint16_t el = 0;
            struct item_list_3 it[2];
            memset(it, 0, sizeof it);
            it[0].buflen = (uint16_t)(sizeof equiv - 1);
            it[0].item_code = LNM$_STRING;
            it[0].bufaddr = equiv;
            it[0].retlen = &el;
            if (!(sys$trnlnm(NULL, &td, &nd, NULL, it) & 1) || el == 0)
                return SS$_NOSUCHDEV;   /* no such logical, or executive absent */
            equiv[el < sizeof equiv ? el : sizeof equiv - 1] = '\0';
            /* A process-permanent file names its device after the ESC NUL IFI
             * header ($GETDVI of SYS$COMMAND is the terminal, rd vms-b14e). */
            if (vms_lnm_is_ppf(equiv, el)) {
                memmove(equiv, equiv + VMS_LNM_PPF_HDR, (size_t)el - VMS_LNM_PPF_HDR);
                equiv[el - VMS_LNM_PPF_HDR] = '\0';
            }
        }

        strncpy(cur, equiv, sizeof(cur) - 1);
        cur[sizeof(cur) - 1] = '\0';
    }
    return SS$_NOSUCHDEV;   /* translation loop -- refuse, do not guess */
}

/*
 * sys$getdvi - Get Device/Volume Information.
 *
 * Either chan (an assigned channel) or devnam (a name descriptor) identifies
 * the device; devnam takes priority when both are supplied. The device is
 * resolved in the EXECUTIVE'S table, so the row is the same one every process
 * on the node sees. A device the executive does not have is SS$_NOSUCHDEV; a
 * channel this process does not hold is SS$_IVCHAN -- neither is invented
 * here, both come back from the executive.
 *
 * @param efn      Event flag (ignored -- synchronous)
 * @param chan     I/O channel (0 if using devnam)
 * @param devnam   Descriptor of device name (NULL if using chan)
 * @param itmlst   Item list of DVI$_ codes to retrieve
 * @param iosb     Optional I/O status block
 * @param astadr   AST completion routine (ignored)
 * @param astprm   AST parameter (ignored)
 * @param nullarg  Reserved, pass 0
 */

/*
 * dvi_parse_devnam - the device part of a $GETDVI device-name argument, as
 * VMS reads it (rd vms-de3a, docs/oracle/semantics/info/): a file
 * specification's device field is accepted ("NLA0:[DIR]FILE.TXT" -> "NLA0:"),
 * the name is case-blind, an empty name is SS$_IVLOGNAM and a character no
 * device or logical name can hold is SS$_IVDEVNAM.
 */
static uint32_t dvi_parse_devnam(const struct dsc$descriptor_s *d, char *out, size_t sz)
{
    size_t n = 0;
    if (!d || !d->dsc$a_pointer || d->dsc$w_length == 0)
        return SS$_IVLOGNAM;
    for (unsigned i = 0; i < d->dsc$w_length; i++) {
        char c = d->dsc$a_pointer[i];
        if (c == ':') {
            if (n + 1 < sz) out[n++] = ':';
            break;
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '$' || c == '_' || c == '-'))
            return SS$_IVDEVNAM;
        if (n + 1 < sz) out[n++] = (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    out[n] = '\0';
    if (n == 0 || (n == 1 && out[0] == ':'))
        return SS$_IVLOGNAM;
    return SS$_NORMAL;
}

static uint32_t getdvi_impl(uint32_t efn, uint16_t chan,
                    struct dsc$descriptor_s *devnam,
                    void *itmlst, struct _iosb *iosb,
                    void (*astadr)(uint32_t), uint32_t astprm,
                    uint32_t nullarg)
{
    (void)efn; (void)astadr; (void)astprm; (void)nullarg;

    if (!itmlst)
        return SS$_BADPARAM;

    /* Extract the device name, if one was given. */
    char devnam_str[VMS_DEVNAM_SIZE] = "";
    int have_name = 0;
    /* A channel, when given, names the device and the name is ignored (VMS --
     * docs/oracle/semantics/info/ DVI.CHAN.AND.NAME, rd vms-de3a). */
    if (chan == 0 && devnam) {
        uint32_t pst = dvi_parse_devnam(devnam, devnam_str, sizeof(devnam_str));
        if (pst != SS$_NORMAL)
            return pst;
        have_name = 1;
    }

    if (!have_name && chan == 0)
        return SS$_BADPARAM;

    /* Ask the executive for the row. */
    struct vms_devinfo info;
    uint32_t status;
    if (have_name) {
        status = device_lookup_translated(devnam_str, &info);
    } else {
        /*
         * A channel number is process-local (sys$assign records it in the PCB;
         * the executive never saw it unless $ASSIGN bound an executive channel).
         * Translate it: the executive's own channel if one was bound, else the
         * device the slot was assigned by name. A slot this process does not
         * hold is SS$_IVCHAN, as on VMS.
         */
        const char *dn = vms$$chan_devnam(chan);
        uint32_t ec = dn ? 0 : vms$$chan_exec_chan(chan);
        if (dn)
            status = device_lookup_translated(dn, &info);
        else if (ec)
            status = vms_kif_getdvi_chan(ec, &info);
        else
            status = SS$_IVCHAN;
    }

    if (status == SS$_NORMAL) {
        info.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

        /* Walk the item list, filling each from the executive's row. */
        const struct item_list_3 *items = (const struct item_list_3 *)itmlst;
        for (; items->buflen != 0 || items->item_code != 0; items++)
            fill_dvi_item(items, &info);
    }
    /*
     * On failure nothing is filled: the caller learns the device could not be
     * read from $STATUS / the IOSB, and no item receives an invented value.
     */

    if (iosb) {
        iosb->iosb$w_status = (uint16_t)status;
        iosb->iosb$w_bcnt   = 0;
    }
    return status;
}

/*
 * sys$getdvi - public entry: the getdvi body above, then the completion every
 * asynchronous system service owes its caller on success -- IOSB written, event
 * flag set, AST queued (vms$$async_finish, sys_ast.c). The body is synchronous,
 * so the request completes before it returns; what it used to omit was telling
 * the caller. A program that waits on the event flag its AST sets (Eight-Cubed
 * sys_getjpi.c) hung forever on a flag nothing ever set.
 */
uint32_t sys$getdvi(uint32_t efn, uint16_t chan,
                    struct dsc$descriptor_s *devnam,
                    void *itmlst, struct _iosb *iosb,
                    void (*astadr)(uint32_t), uint32_t astprm,
                    uint32_t nullarg)
{
    uint32_t rq = vms$$async_begin(efn);
    if (rq != SS$_NORMAL)
        return rq;
    uint32_t st = getdvi_impl(efn, chan, devnam, itmlst, iosb, astadr, astprm, nullarg);
    return vms$$async_finish(efn, iosb, st, astadr, astprm);
}

/*
 * sys$getdviw - Get Device/Volume Information (synchronous wait variant).
 *
 * Identical to sys$getdvi -- our implementation is already synchronous.
 */
uint32_t sys$getdviw(uint32_t efn, uint16_t chan,
                     struct dsc$descriptor_s *devnam,
                     void *itmlst, struct _iosb *iosb,
                     void (*astadr)(uint32_t), uint32_t astprm,
                     uint32_t nullarg)
{
    return sys$getdvi(efn, chan, devnam, itmlst, iosb, astadr, astprm, nullarg);
}

/*
 * vms$$wild_match - Minimal VMS wildcard matcher (case-insensitive).
 *
 * Supports '*' (any sequence, including empty) and '%' (exactly one
 * character) -- the wildcard characters documented for the wildnam argument
 * of $DEVICE_SCAN in the OpenVMS System Services Reference Manual.
 */
static int vms$$wild_match(const char *pat, const char *str)
{
    if (!pat || !*pat) return 1;  /* empty pattern matches everything */

    if (*pat == '*') {
        if (vms$$wild_match(pat + 1, str)) return 1;
        for (; *str; str++)
            if (vms$$wild_match(pat + 1, str + 1)) return 1;
        return 0;
    }
    if (*pat == '%') {
        return *str && vms$$wild_match(pat + 1, str + 1);
    }
    if (!*str) return 0;

    char pc = (*pat >= 'a' && *pat <= 'z') ? (char)(*pat - 'a' + 'A') : *pat;
    char sc = (*str >= 'a' && *str <= 'z') ? (char)(*str - 'a' + 'A') : *str;
    if (pc != sc) return 0;

    return vms$$wild_match(pat + 1, str + 1);
}

/* Copy a name with its trailing colon stripped, upper-cased, for matching. */
static void strip_for_match(char *dst, size_t dstsz, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < dstsz && src[i] != ':'; i++) {
        char c = src[i];
        dst[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    dst[i] = '\0';
}

/*
 * sys$device_scan - Scan for devices matching a wildcard name / item filter.
 *
 * See the doc comment in starlet.h for the overall contract. ctx is a
 * caller-zeroed GENERIC_64 used as an opaque cursor:
 *   gen64$l_longword[0] -- the next EXECUTIVE scan index to examine.
 *   gen64$l_longword[1] -- "matched at least one device so far", used to
 *                          distinguish SS$_NOSUCHDEV (nothing ever matched)
 *                          from SS$_NOMOREDEV (matched before, now exhausted).
 *
 * The device rows come from the executive (vms_kif_devscan); the wildcard and
 * DVS$_DEVCLASS filter are applied here to those rows, exactly as $DEVICE_SCAN
 * filters the I/O database. A per-process program can no longer enumerate a
 * device the executive does not have.
 */
uint32_t sys$device_scan(struct dsc$descriptor_s *devnam, uint16_t *devnamlen,
                         const struct dsc$descriptor_s *wildnam,
                         void *itmlst, GENERIC_64 *ctx)
{
    if (!devnam || !devnam->dsc$a_pointer || !ctx) return SS$_BADPARAM;

    /* Wildcard pattern (default '*'), colon-stripped and upper-cased. */
    char pattern[VMS_DEVNAM_SIZE] = "*";
    if (wildnam && wildnam->dsc$a_pointer && wildnam->dsc$w_length > 0) {
        char raw[VMS_DEVNAM_SIZE];
        size_t plen = wildnam->dsc$w_length;
        if (plen > sizeof(raw) - 1) plen = sizeof(raw) - 1;
        memcpy(raw, wildnam->dsc$a_pointer, plen);
        raw[plen] = '\0';
        strip_for_match(pattern, sizeof(pattern), raw);
    }

    /* Optional DVS$_DEVCLASS filter from itmlst (single-entry item list). */
    int have_class_filter = 0;
    uint32_t class_filter = 0;
    if (itmlst) {
        const struct item_list_3 *items = (const struct item_list_3 *)itmlst;
        for (; items->buflen != 0 || items->item_code != 0; items++) {
            if (items->item_code == DVS$_DEVCLASS && items->bufaddr) {
                have_class_filter = 1;
                class_filter = *(const uint32_t *)items->bufaddr;
                break;
            }
        }
    }

    uint32_t idx = ctx->gen64$l_longword[0];
    uint32_t matched_any = ctx->gen64$l_longword[1];

    for (;;) {
        struct vms_devinfo info;
        uint32_t st = vms_kif_devscan(&idx, &info);

        if (st == SS$_NOMOREDEV) {
            ctx->gen64$l_longword[0] = idx;
            return matched_any ? SS$_NOMOREDEV : SS$_NOSUCHDEV;
        }
        if (st != SS$_NORMAL) {
            /* An ioctl-level failure or an executive that could not answer.
             * Report it as-is; do not fabricate a device to cover it up. */
            ctx->gen64$l_longword[0] = idx;
            return st;
        }

        info.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

        if (have_class_filter && info.devclass != class_filter)
            continue;

        char cmpname[VMS_DEVNAM_SIZE];
        strip_for_match(cmpname, sizeof(cmpname), info.devnam);
        if (!vms$$wild_match(pattern, cmpname))
            continue;

        /* Match: hand the caller this device name (colon-stripped, as the
         * old contract and the tier1 corpus example expect). */
        uint16_t len = (uint16_t)strlen(cmpname);
        if (len > devnam->dsc$w_length) len = devnam->dsc$w_length;
        memcpy(devnam->dsc$a_pointer, cmpname, len);
        if (devnamlen) *devnamlen = len;

        ctx->gen64$l_longword[0] = idx;   /* devscan already advanced idx */
        ctx->gen64$l_longword[1] = 1;
        return SS$_NORMAL;
    }
}

/*
 * sys$alloc - Allocate a device to the calling process.
 *
 * devnam may be a physical unit or a logical name (translated through LNM$SYSTEM
 * as $GETDVI does) and may carry the leading underscore of a physical name. The
 * executive records the claim; phybuf receives the physical name ("_UNIT:").
 */
uint32_t sys$alloc(const struct dsc$descriptor_s *devnam, uint16_t *phylen,
                   struct dsc$descriptor_s *phybuf, uint32_t acmode,
                   uint32_t flags)
{
    (void)acmode; (void)flags;
    if (!devnam || !devnam->dsc$a_pointer || devnam->dsc$w_length == 0)
        return SS$_BADPARAM;

    char name[VMS_DEVNAM_SIZE];
    dsc$strncpy(name, devnam, sizeof(name));
    const char *n = (name[0] == '_') ? name + 1 : name;

    struct vms_devinfo info;
    uint32_t st = device_lookup_translated(n, &info);
    if (!(st & 1))
        return st;
    info.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

    st = vms_kif_alloc(info.devnam);
    if (!(st & 1))
        return st;

    if (phybuf && phybuf->dsc$a_pointer) {
        char phys[VMS_DEVNAM_SIZE + 2];
        int len = snprintf(phys, sizeof(phys), "_%s", info.devnam);
        if (len > 0) {
            uint16_t l = (uint16_t)len;
            (void)lib$scopy_r_dx(&l, phys, phybuf);
            if (phylen) *phylen = l < phybuf->dsc$w_length || phybuf->dsc$b_class == DSC$K_CLASS_D
                                      ? l : phybuf->dsc$w_length;
        }
    }
    return SS$_NORMAL;
}

/* sys$dalloc - Deallocate a device allocated with $ALLOC. */
uint32_t sys$dalloc(const struct dsc$descriptor_s *devnam, uint32_t acmode)
{
    (void)acmode;
    if (!devnam || !devnam->dsc$a_pointer || devnam->dsc$w_length == 0)
        return SS$_BADPARAM;

    char name[VMS_DEVNAM_SIZE];
    dsc$strncpy(name, devnam, sizeof(name));
    const char *n = (name[0] == '_') ? name + 1 : name;

    struct vms_devinfo info;
    uint32_t st = device_lookup_translated(n, &info);
    if (!(st & 1))
        return st;
    info.devnam[VMS_DEVNAM_SIZE - 1] = '\0';
    return vms_kif_dalloc(info.devnam);
}
