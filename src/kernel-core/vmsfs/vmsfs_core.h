/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vmsfs_core.h - substrate-neutral contract for the OVMX ODS-2 core algorithms
 * (rd vms-544, epic vms-8e8).
 *
 * This header declares the pure ODS-2 algorithms promoted into
 * src/kernel-core/vmsfs/ (version resolution + retrieval-map math) and the
 * ODS-2 name-shape constants they and the Linux VFS glue share. It is the ODS-2
 * analog of the executive core's shared declarations: it names NO host type and
 * includes NO <linux/...> header -- only the kernel-backend shim (for the type /
 * string / error vocabulary) and the already-substrate-neutral on-disk format.
 *
 * Both worlds include it:
 *   - the core .c files (vmsfs_version.c, vmsfs_map.c) implement these against
 *     the shim, 0 direct <linux/> deps;
 *   - the Linux VFS glue header src/kernel/vmsfs/vmsfs.h includes it so the rest
 *     of vmsfs.ko sees the same constants and prototypes.
 *
 * vmsfs_ondisk.h currently lives beside the Linux glue in src/kernel/vmsfs/ and
 * is resolved here via the build's -I path -- exactly as the executive core's
 * relocated files reach vms_internal.h, which likewise stayed in src/kernel.
 * The on-disk format is already substrate-neutral (its only host branch,
 * <linux/types.h> under __KERNEL__, is not taken on a NetBSD kernel build,
 * which falls to the <stdint.h> branch); its physical relocation into
 * kernel-core is a separable, zero-algorithm move left to a later phase to
 * avoid churning its ~20 userspace includers here.
 */

#ifndef OVMX_VMSFS_CORE_H
#define OVMX_VMSFS_CORE_H

#include "vmsfs_backend.h"   /* fixed-width types, string/ctype, VMSFS_E* */
#include "vmsfs_ondisk.h"    /* struct vmsfs_retrieval_ptr, VMSFS_MAX_RETRIEVAL */

/* ================================================================
 * ODS-2 name-shape limits (shared by the core algorithms and the VFS glue).
 * ================================================================ */

#define VMSFS_MAX_VERSION 32767
#define VMSFS_MAX_NAME    39          /* ODS-2 name component limit */
#define VMSFS_MAX_TYPE    39          /* ODS-2 type component limit */

/* Maximum length of a full versioned filename: name.type;version */
#define VMSFS_MAX_FILENAME (VMSFS_MAX_NAME + 1 + VMSFS_MAX_TYPE + 1 + 5)

/* ================================================================
 * Version resolution (vmsfs_version.c)
 * ================================================================ */

/*
 * Parse a VMS-style versioned filename into base name and version.
 *
 * Examples:
 *   "FOO.TXT;3"  -> base="FOO.TXT", version=3
 *   "FOO.TXT;0"  -> base="FOO.TXT", version=0  (means highest)
 *   "FOO.TXT"    -> base="FOO.TXT", version=0  (means highest)
 *
 * Returns 0 on success, negative errno on error.
 */
int vmsfs_parse_version(const char *name, char *base, size_t base_size,
                        int *version);

/*
 * Construct a versioned filename "BASE;N" from base name and version.
 * Returns 0 on success, negative errno on error.
 */
int vmsfs_build_versioned_name(const char *base, int version,
                               char *result, size_t size);

/* ================================================================
 * Retrieval-map math (vmsfs_map.c)
 *
 * A file's data is addressed by 1-based virtual block numbers (VBN). Each
 * retrieval pointer maps a contiguous run of VBNs to volume logical blocks
 * (LBN). These functions are pure arithmetic over the in-core retrieval-pointer
 * array (the host-endian cache of struct vmsfs_file_header's fh_map[]).
 * ================================================================ */

/*
 * Translate a 1-based VBN to its LBN by walking the retrieval-pointer array.
 * Returns 0 and stores the LBN in *lbn_out on success, or -VMSFS_EIO when the
 * VBN is not mapped by any pointer.
 */
int vmsfs_vbn_to_lbn(const struct vmsfs_retrieval_ptr *map, uint16_t map_count,
                     uint32_t vbn, uint32_t *lbn_out);

/*
 * The next unallocated VBN for a file (sum of all mapped block counts + 1).
 */
uint32_t vmsfs_next_vbn(const struct vmsfs_retrieval_ptr *map,
                        uint16_t map_count);

/*
 * Add one block (@lbn) to a file's retrieval map. Extends the last pointer in
 * place when @lbn is contiguous with it; otherwise appends a new pointer.
 * Updates *map_count. Returns 0, or -VMSFS_ENOSPC when the map array is full.
 */
int vmsfs_extend_map(struct vmsfs_retrieval_ptr *map, uint16_t *map_count,
                     uint32_t lbn);

/* ================================================================
 * Filename FORMAT algorithms (vmsfs_name.c, rd vms-00c)
 *
 * Pure ODS-2 name-shape operations over caller-provided byte buffers. They
 * name no host object and use only the shim's freestanding string/ctype
 * vocabulary, so both the Linux VFS glue and (later) the NetBSD vnode backend
 * call them identically.
 * ================================================================ */

/*
 * Split "FOO.TXT" into name="FOO" and type="TXT". If there is no dot (or the
 * only dot is a leading one), type is set to the empty string. Both outputs are
 * truncated to fit their buffers.
 */
void vmsfs_split_name_type(const char *fullname,
                           char *name, size_t name_size,
                           char *type, size_t type_size);

/*
 * Uppercase a NUL-terminated string in place (VMS/ODS-2 convention).
 */
void vmsfs_strupper(char *s);

/*
 * Compare a directory-entry name (@de_name, @de_name_len) against a target
 * (@target, @target_len). @case_blind selects case-insensitive comparison.
 * Returns true on an exact (length + content) match.
 */
bool vmsfs_name_match(const char *de_name, uint8_t de_name_len,
                      const char *target, unsigned int target_len,
                      int case_blind);

#endif /* OVMX_VMSFS_CORE_H */
