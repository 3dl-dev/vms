#ifndef __RMS_RMS_H
#define __RMS_RMS_H

/*
 * RMS Public Interface - Master include for Record Management Services
 *
 * This header provides the complete RMS API for VMS-compatible
 * file and record operations on the OVMX system.
 */

#include <stdint.h>

#include "fab.h"
#include "rab.h"
#include "xab.h"
#include "nam.h"

#include "rmsdef.h"
#include "ssdef.h"

/*
 * Every RMS service takes the VMS three-argument form  SYS$xxx cb ,[err] ,[suc]
 * (VSI OpenVMS Record Management Services Reference Manual, Part III):
 *   cb  — control-block address (FAB for file ops, RAB for record ops)
 *   err — AST-level error   completion routine entry mask (or 0/NULL)
 *   suc — AST-level success completion routine entry mask (or 0/NULL)
 * OVMX RMS completes synchronously; a supplied completion routine is invoked
 * with the control-block address before the service returns.
 */

/* File-level operations */
uint32_t sys$open(void *fab, void (*err)(void *), void (*suc)(void *));       /* Open existing file */
uint32_t sys$close(void *fab, void (*err)(void *), void (*suc)(void *));      /* Close file */
uint32_t sys$create(void *fab, void (*err)(void *), void (*suc)(void *));     /* Create new file */
uint32_t sys$erase(void *fab, void (*err)(void *), void (*suc)(void *));      /* Delete file */
uint32_t sys$display(void *fab, void (*err)(void *), void (*suc)(void *));    /* Display file attributes */
uint32_t sys$extend(void *fab, void (*err)(void *), void (*suc)(void *));     /* Extend file allocation */

/* Stream (record context) operations */
uint32_t sys$connect(void *rab, void (*err)(void *), void (*suc)(void *));    /* Connect RAB to FAB (open stream) */
uint32_t sys$disconnect(void *rab, void (*err)(void *), void (*suc)(void *)); /* Disconnect RAB from FAB */

/* Record-level operations */
uint32_t sys$get(void *rab, void (*err)(void *), void (*suc)(void *));        /* Read a record */
uint32_t sys$put(void *rab, void (*err)(void *), void (*suc)(void *));        /* Write a record */
uint32_t sys$update(void *rab, void (*err)(void *), void (*suc)(void *));     /* Update current record */
uint32_t sys$delete(void *rab, void (*err)(void *), void (*suc)(void *));     /* Delete current record */
uint32_t sys$find(void *rab, void (*err)(void *), void (*suc)(void *));       /* Position to record without reading */

/* Filespec operations */
uint32_t sys$parse(void *fab, void (*err)(void *), void (*suc)(void *));      /* Parse filespec into NAM block */
uint32_t sys$search(void *fab, void (*err)(void *), void (*suc)(void *));     /* Search for next wildcard match */

/*
 * rms_search_fid (OVMX, vms-481) - read the genuine ODS-2 File ID of the most
 * recent $SEARCH match from a NAM block's active wildcard context. On the
 * __linux__ ACP backend the FID comes from the executive directory search
 * (IO$_ACPCONTROL); DCL DIRECTORY /FULL uses it to print the real File ID
 * "(num,seq,rvn)". Returns 1 (and fills the outputs) when a match FID is
 * available, 0 otherwise (no context, or the non-__linux__ POSIX passthrough,
 * which has no executive FID). Not a VMS-authentic service name -- an OVMX
 * accessor over the NAM the search fills.
 */
int rms_search_fid(void *nam, uint16_t *num, uint16_t *seq,
                   uint8_t *rvn, uint8_t *nmx);

/* rms_search_end (OVMX, vms-481) - release a NAM's wildcard search context and
 * its executive channel without iterating to RMS$_NMF (early-stop cleanup). */
void rms_search_end(void *nam);

/*
 * rms_file_attr (OVMX, vms-481) - read a file's genuine ODS-2 header attributes
 * through the Files-11 ACP ($ASSIGN + IO$_ACCESS + read the ATR list, then
 * IO$_DEACCESS). Serves DCL DIRECTORY /FULL (real File ID + size + dates +
 * protection) and the F$FILE_ATTRIBUTES lexical (EOF/ALQ/RFM/RAT/MRS/dates)
 * from the on-disk header the ACP decodes -- NOT stat() on a /vms passthrough.
 * On the non-__linux__ netbsd-vax cross it falls back to stat() (no ACP yet,
 * vms-d5d). Returns RMS$_NORMAL (out filled), or a fail-honest RMS$_ (FNF/DNF/
 * ACC) with *out zeroed. Not a VMS-authentic service name -- an OVMX accessor.
 */
struct rms_fileattr {
    uint16_t fid_num, fid_seq;   /* genuine File ID {NUM,SEQ,RVN} from the ACP  */
    uint8_t  fid_rvn, fid_nmx;
    uint16_t version;            /* resolved version (0 asked => highest)       */
    uint32_t efblk;              /* end-of-file VBN (EOF = (efblk-1)*512+ffbyte) */
    uint32_t hiblk;              /* highest allocated VBN (allocation quantity)  */
    uint16_t ffbyte;             /* first free byte in the EOF block            */
    uint16_t fileprot;           /* ODS-2 protection, 4 nibbles S/O/G/W         */
    uint16_t uic_group, uic_member;
    uint8_t  rfm;                /* record format (FAT fat_rtype == FAB$C_*)    */
    uint8_t  rat;                /* record attributes (FAT fat_rattrib)         */
    uint16_t mrs;               /* max/record size (FAT fat_rsize)              */
    uint8_t  is_directory;       /* 1 if the file characteristics say directory */
    uint8_t  credate[8];         /* VMS 64-bit absolute creation time           */
    uint8_t  revdate[8];         /* VMS 64-bit absolute revision time           */
};
uint32_t rms_file_attr(const char *vmsspec, struct rms_fileattr *out);

/*
 * rms_stage_over_acp (OVMX, vms-104) - read `vmsspec`'s GENUINE bytes off the
 * mounted ODS-2 volume THROUGH the executive Files-11 ACP and write them to the
 * Linux path `destpath` (mode 0755). The bytes come from IO$_READVBLK over
 * /dev/vms -- NEVER a /vms POSIX read (Rule 9 / INV-6) -- so a native bootstrap
 * tool the kernel execve()s (TCC/LIBRARIAN/LINK.EXE) or a first-hop image gets a
 * POSIX home sourced from the volume. `vmsspec` is an already-effective filespec
 * (device/dir defaulted). Returns RMS$_NORMAL; a fail-honest RMS$_ (FNF/...) when
 * the ACP answered but the file is absent; RMS$_ACC when no ACP-mounted volume is
 * reachable (caller must NOT fall back to /vms); SS$_ABORT on a dest write error.
 */
uint32_t rms_stage_over_acp(const char *vmsspec, const char *destpath);

/* rms_executive_absent (OVMX, vms-5f0) - 1 when /dev/vms / the Files-11 ACP is
 * unreachable (host ctest, plain-container gates, netbsd-vax cross), 0 when the
 * executive is present. The single source DCL's host-defer shares with RMS's own
 * $OPEN/$SEARCH defers so DCL runs its legacy resolver ONLY when RMS also would
 * (Rule 9 / INV-6: executive present => no legacy /vms fall-back). */
int rms_executive_absent(void);

/* Positioning and I/O control */
uint32_t sys$rewind(void *rab, void (*err)(void *), void (*suc)(void *));     /* Rewind to beginning of file */
uint32_t sys$flush(void *rab, void (*err)(void *), void (*suc)(void *));      /* Flush buffers to disk */

#endif /* __RMS_RMS_H */
