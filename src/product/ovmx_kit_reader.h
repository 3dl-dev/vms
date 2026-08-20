/*
 * ovmx_kit_reader.h - shared reader for the OVMX product kit container
 * (src/libvms/include/ovmx_kit_format.h, vms-0b6).
 *
 * WHY THIS EXISTS (vms-df9). `ovmx_kit_format.h`'s own header comment is
 * explicit that reading a kit to install/register a product is vms-df9's
 * job, not the format header's. Before this file, the ONLY code that
 * opened/validated/read a kit was three static functions private to
 * tools/ovmx_kit_pack.c (reader_open/read_entries + the per-file
 * checksum-verified read inlined in do_extract()). PRODUCT.EXE
 * (src/product/product.c) needs the exact same open/validate/read-entries/
 * read-one-file-verified sequence to install a kit's payload -- duplicating
 * those ~80 lines a second time is exactly the "hand-roll a second kit
 * parser" vms-df9's spec forbids. This header + ovmx_kit_reader.c is the
 * single shared implementation; tools/ovmx_kit_pack.c's list/extract modes
 * were refactored to call it too, so there is now exactly one kit reader in
 * the tree, matching the "one shared description" pattern vmsfs_ondisk.h /
 * known_images.h already established for their own formats.
 *
 * Deliberately minimal and standalone: only <stdint.h>/<stddef.h> and
 * ovmx_kit_format.h (itself dependency-free beyond ovmx_fileprot.h), so
 * linking it pulls in nothing else -- both ovmx_kit_pack (factory build
 * tooling, minimal deps by design) and PRODUCT.EXE (a hosted SYS$SYSTEM:
 * utility, links libvmsfs for protection/UIC conversion) can use it without
 * dragging the other's dependency graph along.
 */
#ifndef OVMX_KIT_READER_H
#define OVMX_KIT_READER_H

#include <stdint.h>
#include <stddef.h>

#include "ovmx_kit_format.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OVMX_KIT_READER_OK       = 0,
    OVMX_KIT_READER_ERR_OPEN    = -1,  /* open()/read() of the kit file itself failed */
    OVMX_KIT_READER_ERR_NOTKIT  = -2,  /* bad magic -- not an OVMX kit file */
    OVMX_KIT_READER_ERR_CHKSUM  = -3,  /* header or content checksum mismatch */
    OVMX_KIT_READER_ERR_READ    = -4,  /* short read of the index or a file's content */
    OVMX_KIT_READER_ERR_NOMEM   = -5,  /* allocation failure */
    OVMX_KIT_READER_ERR_BADSPEC = -6,  /* malformed target filespec (see relpath below) */
} ovmx_kit_reader_err_t;

/*
 * The byte source a kit is pulled from. Two backends exist, and the reader
 * body (index + per-file reads, checksum verification) is identical over both:
 *
 *   - POSIX fd (ovmx_kit_reader_open) -- factory build tooling
 *     (tools/ovmx_kit_pack.c) and the executive-absent host ctest defer, where
 *     a kit is a plain Linux file.
 *   - An ACP-routed RMS handle (ovmx_kit_reader_open_source, caller-supplied)
 *     -- PRODUCT.EXE at RUNTIME reads its /SOURCE kit off the MOUNTed
 *     distribution volume by VMS filespec over the Files-11 ACP (vms-3a8),
 *     never a /vms POSIX passthrough (Rule 9 / INV-6). The RMS backend lives in
 *     src/product/product.c so this module keeps its minimal, RMS-free
 *     dependency graph (the host packer must not drag in libvmsrms).
 *
 * @pread reads EXACTLY @n bytes at absolute byte offset @off into @buf,
 * returning 0 on success or -1 on any short read/error (positioned I/O, no
 * cursor state shared with the reader). @close releases @ctx and is called
 * once by ovmx_kit_reader_close(); either may be NULL.
 */
typedef struct ovmx_kit_source {
    void *ctx;
    int  (*pread)(void *ctx, void *buf, size_t n, uint64_t off);
    void (*close)(void *ctx);
} ovmx_kit_source_t;

typedef struct {
    ovmx_kit_source_t src;
    struct ovmx_kit_header hdr;
} ovmx_kit_reader_t;

/*
 * Open @kitfile as a plain POSIX file, validate the magic and header checksum,
 * and cache the header in *r. Returns OVMX_KIT_READER_OK or a negative
 * ovmx_kit_reader_err_t. On error, *r is left with a NULL source (safe to call
 * ovmx_kit_reader_close() unconditionally).
 */
int ovmx_kit_reader_open(ovmx_kit_reader_t *r, const char *kitfile);

/*
 * Bind an already-opened, caller-supplied byte @source (e.g. an ACP-routed RMS
 * handle), validate the magic and header checksum, and cache the header in *r.
 * The reader takes ownership of @source->ctx: on success ovmx_kit_reader_close()
 * will call @source->close; on failure this function calls it before returning,
 * so the caller never double-frees. Returns OVMX_KIT_READER_OK or a negative
 * ovmx_kit_reader_err_t.
 */
int ovmx_kit_reader_open_source(ovmx_kit_reader_t *r, const ovmx_kit_source_t *source);

/*
 * Read the r->hdr.kh_file_count-entry index into a freshly calloc'd array.
 * Caller owns *out and must free() it. Returns OVMX_KIT_READER_OK or a
 * negative ovmx_kit_reader_err_t.
 */
int ovmx_kit_reader_entries(ovmx_kit_reader_t *r, struct ovmx_kit_entry **out);

/*
 * Read one entry's content into a freshly malloc'd buffer, verifying its
 * ke_checksum. *buf_out is set to NULL and *len_out to 0 for a zero-length
 * entry (malloc(0) is not relied on). Caller owns a non-NULL *buf_out and
 * must free() it. Returns OVMX_KIT_READER_OK or a negative
 * ovmx_kit_reader_err_t.
 */
int ovmx_kit_reader_read_file(ovmx_kit_reader_t *r,
                              const struct ovmx_kit_entry *e,
                              uint8_t **buf_out);

/*
 * Reverse a kit entry's target filespec ("SYS$COMMON:[SYSEXE.SUB]FOO.DAT")
 * into a relative path ("SYSEXE/SUB/FOO.DAT") by discarding whatever
 * device/logical precedes the '[' and turning '.'-separated directory
 * components into '/'-separated ones. "[000000]" maps to the bare
 * basename (no directory component). This throws away the device/logical
 * portion deliberately -- both consumers (ovmx_kit_pack's `extract`, which
 * writes under an arbitrary host output directory, and PRODUCT.EXE's
 * INSTALL, which writes under whatever Linux path the /DESTINATION device
 * is mounted at) supply their own root; the kit format never claims to
 * know what "SYS$COMMON:" resolves to on the machine doing the reading.
 *
 * Returns OVMX_KIT_READER_OK or OVMX_KIT_READER_ERR_BADSPEC.
 */
int ovmx_kit_reader_relpath(const char *filespec, char *out, size_t outlen);

void ovmx_kit_reader_close(ovmx_kit_reader_t *r);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_KIT_READER_H */
