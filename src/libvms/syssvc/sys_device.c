/*
 * sys_device.c - Device Information System Services
 *
 * Implements sys$getdvi and sys$getdviw for querying device
 * and volume attributes.  VMS device names are mapped to Linux
 * mount points and character devices.
 *
 * Device name mapping:
 *   SYS$DISK:, DUA0:, $1$DUA0:  -> /
 *   TT:, _TTAn:, _FTAn:         -> terminal (isatty on stdin/stdout)
 *   MBAn:                        -> mailbox virtual device
 *
 * For disk devices, volume information is obtained via statvfs().
 * For terminal devices, termios/ioctl provides terminal properties.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include "starlet.h"
#include "dvidef.h"
#include "dcdef.h"

/*
 * Device type codes — a subset of VMS DT$_ values.
 * These are not defined in a separate header, so we define the
 * subset used by OVMX here.
 */
#define DT$_DISK        26   /* Generic disk */
#define DT$_TERMINAL    66   /* Terminal */
#define DT$_MAILBOX    101   /* Mailbox */
#define DT$_UNKNOWN      0

/*
 * Device status bits (DVI$_STS / device characteristics DVI$_DEVCHAR).
 * These are a subset of VMS DEV$M_ values.
 */
#define DEV$M_MNT    0x00000200  /* Device is mounted */
#define DEV$M_ALL    0x00000008  /* Device is allocated */
#define DEV$M_AVL    0x00000020  /* Device is available */
#define DEV$M_SHR    0x00000010  /* Device is shareable */

/*
 * Classify a VMS device name into a device class.
 *
 * Handles:
 *   TT:, _TTAn:, _FTAn:   -> DC$_TERM
 *   MBAn:                   -> DC$_MAILBOX
 *   SYS$DISK:, DUAn:, etc. -> DC$_DISK
 */
static int classify_device(const char *devnam, char *linux_path, size_t pathsz)
{
    if (!devnam || !devnam[0]) {
        if (linux_path) {
            strncpy(linux_path, "/", pathsz);
            linux_path[pathsz - 1] = '\0';
        }
        return DC$_DISK;
    }

    /* Uppercase comparison prefix */
    char upper[64];
    strncpy(upper, devnam, sizeof(upper) - 1);
    upper[sizeof(upper) - 1] = '\0';
    for (size_t i = 0; upper[i]; i++)
        if (upper[i] >= 'a' && upper[i] <= 'z')
            upper[i] = (char)(upper[i] - 'a' + 'A');

    /* Strip trailing colon */
    size_t ulen = strlen(upper);
    if (ulen > 0 && upper[ulen - 1] == ':')
        upper[ulen - 1] = '\0';

    /* Terminal devices */
    if (strncmp(upper, "TT",   2) == 0 ||
        strncmp(upper, "_TTA", 4) == 0 ||
        strncmp(upper, "_FTA", 4) == 0) {
        if (linux_path) {
            strncpy(linux_path, "/dev/tty", pathsz);
            linux_path[pathsz - 1] = '\0';
        }
        return DC$_TERM;
    }

    /* Mailbox */
    if (strncmp(upper, "MBA", 3) == 0) {
        if (linux_path) {
            linux_path[0] = '\0';
        }
        return DC$_MAILBOX;
    }

    /* Disk: SYS$DISK, DUA, DKA, $1$DUA, etc. — map to root filesystem */
    if (linux_path) {
        strncpy(linux_path, "/", pathsz);
        linux_path[pathsz - 1] = '\0';
    }
    return DC$_DISK;
}

/*
 * Fill a single DVI item from device information.
 */
static void fill_dvi_item(const struct item_list_3 *item,
                           int devclass, int devtype,
                           const char *devnam_str,
                           const char *linux_path)
{
    switch (item->item_code) {

        case DVI$_DEVNAM: {
            uint16_t len = (uint16_t)strlen(devnam_str);
            if (len > item->buflen) len = item->buflen;
            if (item->bufaddr) memcpy(item->bufaddr, devnam_str, len);
            if (item->retlen) *item->retlen = len;
            break;
        }

        case DVI$_FULLDEVNAM: {
            uint16_t len = (uint16_t)strlen(devnam_str);
            if (len > item->buflen) len = item->buflen;
            if (item->bufaddr) memcpy(item->bufaddr, devnam_str, len);
            if (item->retlen) *item->retlen = len;
            break;
        }

        case DVI$_DEVCLASS: {
            if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                *(uint32_t *)item->bufaddr = (uint32_t)devclass;
            if (item->retlen) *item->retlen = sizeof(uint32_t);
            break;
        }

        case DVI$_DEVTYPE: {
            if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                *(uint32_t *)item->bufaddr = (uint32_t)devtype;
            if (item->retlen) *item->retlen = sizeof(uint32_t);
            break;
        }

        case DVI$_MAXBLOCK: {
            if (devclass == DC$_DISK && linux_path && linux_path[0]) {
                struct statvfs svfs;
                if (statvfs(linux_path, &svfs) == 0) {
                    /* Blocks in 512-byte VMS blocks */
                    uint32_t blks = (uint32_t)(svfs.f_blocks *
                                    (svfs.f_frsize / 512));
                    if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                        *(uint32_t *)item->bufaddr = blks;
                    if (item->retlen) *item->retlen = sizeof(uint32_t);
                    break;
                }
            }
            if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                *(uint32_t *)item->bufaddr = 0;
            if (item->retlen) *item->retlen = sizeof(uint32_t);
            break;
        }

        case DVI$_FREEBLOCKS: {
            if (devclass == DC$_DISK && linux_path && linux_path[0]) {
                struct statvfs svfs;
                if (statvfs(linux_path, &svfs) == 0) {
                    uint32_t free_blks = (uint32_t)(svfs.f_bavail *
                                         (svfs.f_frsize / 512));
                    if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                        *(uint32_t *)item->bufaddr = free_blks;
                    if (item->retlen) *item->retlen = sizeof(uint32_t);
                    break;
                }
            }
            if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                *(uint32_t *)item->bufaddr = 0;
            if (item->retlen) *item->retlen = sizeof(uint32_t);
            break;
        }

        case DVI$_VOLNAM: {
            /* Return a synthetic volume label based on mount point */
            const char *volnam = "OVMX";
            uint16_t len = (uint16_t)strlen(volnam);
            if (len > item->buflen) len = item->buflen;
            if (item->bufaddr) memcpy(item->bufaddr, volnam, len);
            if (item->retlen) *item->retlen = len;
            break;
        }

        case DVI$_MOUNTCNT: {
            /* Always report 1 for mounted devices */
            if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                *(uint32_t *)item->bufaddr = (devclass == DC$_DISK) ? 1 : 0;
            if (item->retlen) *item->retlen = sizeof(uint32_t);
            break;
        }

        case DVI$_DEVCHAR: {
            uint32_t chars = 0;
            if (devclass == DC$_DISK)
                chars = DEV$M_MNT | DEV$M_AVL | DEV$M_SHR;
            else if (devclass == DC$_TERM)
                chars = DEV$M_AVL;
            if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                *(uint32_t *)item->bufaddr = chars;
            if (item->retlen) *item->retlen = sizeof(uint32_t);
            break;
        }

        case DVI$_BLOCKSIZE: {
            /* VMS standard block size is 512 bytes */
            if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                *(uint32_t *)item->bufaddr = 512;
            if (item->retlen) *item->retlen = sizeof(uint32_t);
            break;
        }

        case DVI$_UNIT: {
            if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                *(uint32_t *)item->bufaddr = 0;
            if (item->retlen) *item->retlen = sizeof(uint32_t);
            break;
        }

        case DVI$_AVAILABLE: {
            if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                *(uint32_t *)item->bufaddr = 1;
            if (item->retlen) *item->retlen = sizeof(uint32_t);
            break;
        }

        default:
            /* Unknown item — fill with zeros */
            if (item->bufaddr && item->buflen > 0)
                memset(item->bufaddr, 0,
                       (item->buflen < sizeof(uint32_t)) ?
                       item->buflen : sizeof(uint32_t));
            if (item->retlen) *item->retlen = 0;
            break;
    }
}

/*
 * sys$getdvi - Get Device/Volume Information.
 *
 * Returns device and volume attributes for the specified device.
 * Either chan (channel) or devnam (name descriptor) identifies the device;
 * devnam takes priority in this implementation.
 *
 * @param efn      Event flag (ignored — synchronous)
 * @param chan     I/O channel (0 if using devnam)
 * @param devnam   Descriptor of device name (NULL if using chan)
 * @param itmlst   Item list of DVI$_ codes to retrieve
 * @param iosb     Optional I/O status block
 * @param astadr   AST completion routine (ignored)
 * @param astprm   AST parameter (ignored)
 * @param nullarg  Reserved, pass 0
 */
uint32_t sys$getdvi(uint32_t efn, uint16_t chan,
                    struct dsc$descriptor_s *devnam,
                    void *itmlst, struct _iosb *iosb,
                    void (*astadr)(uint32_t), uint32_t astprm,
                    uint32_t nullarg)
{
    (void)efn; (void)chan; (void)astadr; (void)astprm; (void)nullarg;

    if (!itmlst)
        return SS$_BADPARAM;

    /* Extract device name */
    char devnam_str[64] = "";
    if (devnam && devnam->dsc$a_pointer)
        dsc$strncpy(devnam_str, devnam, sizeof(devnam_str));

    /* Classify the device */
    char linux_path[256];
    int devclass = classify_device(devnam_str, linux_path, sizeof(linux_path));
    int devtype;
    switch (devclass) {
        case DC$_DISK:    devtype = DT$_DISK;     break;
        case DC$_TERM:    devtype = DT$_TERMINAL; break;
        case DC$_MAILBOX: devtype = DT$_MAILBOX;  break;
        default:          devtype = DT$_UNKNOWN;  break;
    }

    /* Walk item list */
    const struct item_list_3 *items = (const struct item_list_3 *)itmlst;
    for (; items->buflen != 0 || items->item_code != 0; items++) {
        fill_dvi_item(items, devclass, devtype, devnam_str, linux_path);
    }

    uint32_t status = SS$_NORMAL;
    if (iosb) {
        iosb->iosb$w_status = (uint16_t)status;
        iosb->iosb$w_bcnt   = 0;
    }
    return status;
}

/*
 * sys$getdviw - Get Device/Volume Information (synchronous wait variant).
 *
 * Identical to sys$getdvi — our implementation is already synchronous.
 */
uint32_t sys$getdviw(uint32_t efn, uint16_t chan,
                     struct dsc$descriptor_s *devnam,
                     void *itmlst, struct _iosb *iosb,
                     void (*astadr)(uint32_t), uint32_t astprm,
                     uint32_t nullarg)
{
    return sys$getdvi(efn, chan, devnam, itmlst, iosb, astadr, astprm, nullarg);
}
