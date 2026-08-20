#ifndef RMS_INTERNAL_H
#define RMS_INTERNAL_H

/*
 * rms_internal.h - Shared internal helpers for RMS subsystem
 *
 * These functions are shared across rms_seq.c, rms_rel.c, and rms_idx.c
 * to avoid code duplication.  They are NOT part of the public RMS API.
 */

#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>

/*
 * rms_complete - RMS completion-routine dispatch.
 *
 * Every RMS service takes the VMS three-argument form SYS$xxx cb,[err],[suc]
 * (VSI OpenVMS RMS Reference Manual, Part III): on success the AST-level
 * success routine (suc) is invoked with the control-block address; on failure
 * the error routine (err) is invoked. OVMX RMS completes synchronously, so the
 * routine (if any) runs before the service returns. Passing NULL for both is
 * the ordinary synchronous call. This keeps completion routines real behaviour
 * rather than a silently-ignored facade.
 *
 * VMS success = odd status (low bit set).
 */
static inline uint32_t rms_complete(uint32_t status, void *cb,
                                    void (*err)(void *), void (*suc)(void *))
{
    if (status & 1u) {
        if (suc) suc(cb);
    } else {
        if (err) err(cb);
    }
    return status;
}

/*
 * rms_read_exact - Read exactly 'count' bytes from fd.
 * Returns number of bytes read, or -1 on error.
 */
ssize_t rms_read_exact(int fd, void *buf, size_t count);

/*
 * rms_write_exact - Write exactly 'count' bytes to fd.
 * Returns 0 on success, -1 on error.
 */
int rms_write_exact(int fd, const void *buf, size_t count);

#if defined(__linux__)
/*
 * Files-11 (ODS-2) ACP directory-FID resolver (defined in rms_core.c, epic
 * vms-208). Walks each "COMP" of a "A.B.C" directory path as "COMP.DIR" from
 * the MFD ([000000], all-zero DID) via IO$_ACCESS on `chan`, returning the
 * final directory's FID as {*dn,*ds,*dr,*dx}. Empty/[000000] path => the MFD
 * (all-zero). Returns SS$_NORMAL or a fail-honest SS$_ (e.g. SS$_NOSUCHFILE).
 * Shared with rms_search.c so $SEARCH reaches the same executive directory the
 * $OPEN/$CREATE path does -- not vmsfs_to_linux_path (CLAUDE.md Rule 9/INV-6).
 */
uint32_t rms_acp_resolve_did(uint32_t chan, const char *dirpath,
                             uint16_t *dn, uint16_t *ds,
                             uint8_t *dr, uint8_t *dx);

/* The boot volume unit RMS $ASSIGNs when a filespec names no device (the
 * mounted SYS$DISK; DKA0: until the discovered-SYS$DISK logical is bound). */
#define RMS_ACP_DEFAULT_DEV "DKA0:"
#endif /* __linux__ */

#endif /* RMS_INTERNAL_H */
