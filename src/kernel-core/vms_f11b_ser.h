/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_f11b_ser.h -- the Files-11 XQP's per-file SERIALIZATION LOCK: its
 * resource name and the rules for its value block (rd vms-d63).
 *
 * WHAT IT IS FOR. A node that caches a file's header (its FCB: the extent map,
 * the EOF, the attributes) must learn when ANOTHER node changes that header.
 * The XQP does it with a lock per file -- "F11B$s" + the file number, a
 * sub-lock of the volume's "F11B$v" + label lock, at (UIC group 0, KERNEL
 * mode) -- taken PW with LCK$M_VALBLK around every operation on the file and
 * released with its 16-byte value block. The value block carries the file's
 * change counters. A node holding a cached header compares the counters it
 * reads on its next grant with the ones it cached; a difference means another
 * node changed the file, and the cache is rebuilt from the disk. That is the
 * generic lock-value-block cache-validation protocol of the OpenVMS
 * Programming Concepts Manual (Lock Management, "Using the Lock Value Block"),
 * applied by the XQP to its file-header cache.
 *
 * rd vms-d63 is what happens without it: OVMX appended to EVAC.DAT, wrote the
 * header's new EOF, and never touched F11B$s -- so VAX1 converted its cached
 * lock, read the SAME counters it cached, trusted its old EOF, and wrote its
 * records over OVMX's (tests/lab/captures/vms-5fa-shared-volume-r2-20261102).
 *
 * EVERYTHING BELOW IS MEASURED, from real VAX traffic only (clean-room, Rule
 * 8: wire captures and SDA output; no VSI/HPE binary or source), by
 * tools/cluster/f11b_ser_valblk_census.py, pinned by ctest
 * cluster_host_f11b_ser_valblk_census:
 *
 *   NAME. 'F11B$s' + the file number as 4 LITTLE-ENDIAN bytes, 10 bytes --
 *     37,340 real requests (rd vms-f4c0's census), and VAX1's own SDA in the
 *     vms-5fa r2 capture (vax1.log: "F11B$s*." Length 10, parent the
 *     F11B$vEVAC5FA lock, Kernel mode, System). The 4 bytes are FID$W_NUM
 *     plus FID$B_NMX << 16 (ODS-2's file number; the corpus never exercises
 *     nmx != 0).
 *
 *   MODE. PW (mode 4) on 37,956 of 38,094 real requests; NL on the rest.
 *
 *   VALUE BLOCK. Two independent counters and eight zero bytes:
 *     bytes 0..3   the HEADER counter: the file's header was changed.
 *     bytes 4..7   the DATA counter: the XQP changed the file's CONTENTS --
 *                  a directory's entries.
 *     bytes 8..15  zero on every real F11B$s value block in the corpus.
 *   Read off one real sequence end to end (tests/lab/captures/
 *   vms-b20-mscp-read-20261011/mscp-read-write.pcap: VAX2 COPYing a file
 *   onto VAX1's MSCP-served DATA2):
 *     - fid 13, a NEW file: granted (0,0); allocation (bitmap LBNs) and its
 *       header (LBN 445654) written; released (2,1) -- the first holder
 *       turns the all-zero block into (1,1), and the header change makes
 *       the header counter 2;
 *     - fid 13 again, granted (2,1): the close's header write (LBN 445654
 *       again); released (3,1) -- header counter +1, data counter unchanged
 *       although 36 data blocks were written meanwhile (RMS's own writes are
 *       not the XQP's);
 *     - fid 4, the MFD the new entry went into: granted (1,3); released
 *       (1,4) -- the data counter +1, the header counter unchanged;
 *     - a file only looked at (fid 8, 3, 9, 2, 7, 5, 6): granted (0,0),
 *       released (1,1) -- the initialisation alone.
 *   Corpus-wide (34,744 real grants carrying a block): bytes 8..15 are
 *   zero on 34,698; between successive grants of one resource the header
 *   counter moved up 1,081 times and the data counter 338, and neither ever
 *   moved down except where the master had re-created the resource (6) --
 *   a forgotten resource's block comes back all zero and the next holder
 *   initialises it to (1,1) again.
 *
 * WHAT IS NOT MEASURED, and is not claimed: how a VAX that holds the lock at
 * NL and converts it up (VAX1's own pattern on a volume it masters, vax1.log:
 * "Granted at NL ... Flags: VALBLK CONVERT") reacts to a header counter it
 * did not cache -- that is the protocol's whole point and the Programming
 * Concepts Manual's, but the rebuild on VAX1 itself is proved only by the
 * lab re-fire (rd vms-5fa S3/S4), not here.
 *
 * Host-neutral and freestanding: fixed-width integers only.
 */
#ifndef VMS_F11B_SER_H
#define VMS_F11B_SER_H

#include "vms_wire_types.h"   /* fixed-width types, substrate-selected once */

#define VMS_F11B_SER_NAME_LEN   10u    /* 'F11B$s' + LE32(file number) */
#define VMS_F11B_SER_VALBLK_LEN 16u
#define VMS_F11B_SER_GROUP      0u     /* UIC group 0: system-wide        */
#define VMS_F11B_SER_ACMODE     0u     /* PSL$C_KERNEL                    */

/* The 10-byte resource name for file number `fidnum` (num | nmx << 16).
 * Returns the length written, or 0 when `cap` is too small. */
static inline uint32_t vms_f11b_ser_name(uint32_t fidnum, uint8_t *out,
                                         uint32_t cap)
{
    if (out == (uint8_t *)0 || cap < VMS_F11B_SER_NAME_LEN)
        return 0u;
    out[0] = 'F'; out[1] = '1'; out[2] = '1'; out[3] = 'B';
    out[4] = '$'; out[5] = 's';
    out[6] = (uint8_t)(fidnum & 0xffu);
    out[7] = (uint8_t)((fidnum >> 8) & 0xffu);
    out[8] = (uint8_t)((fidnum >> 16) & 0xffu);
    out[9] = (uint8_t)((fidnum >> 24) & 0xffu);
    return VMS_F11B_SER_NAME_LEN;
}

static inline uint32_t vms_f11b_ser_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void vms_f11b_ser_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

/* The header counter a grant handed out -- what a cached header is valid
 * against. */
static inline uint32_t vms_f11b_ser_hdr_seq(const uint8_t *vb)
{
    return vms_f11b_ser_rd32(vb);
}

static inline uint32_t vms_f11b_ser_data_seq(const uint8_t *vb)
{
    return vms_f11b_ser_rd32(vb + 4);
}

/*
 * The block a release writes, from the block the grant handed out: an
 * all-zero block becomes (1,1) (the first holder's initialisation), then the
 * header counter steps if this holder changed the header and the data counter
 * if it changed the XQP-maintained contents. A counter never steps to 0 --
 * 0 is what a forgotten resource reads as, and a node that cached 0 must
 * still see a change. Bytes 8..15 are written zero, as every real block has
 * them.
 */
static inline void vms_f11b_ser_next(uint8_t *vb, int hdr_changed,
                                     int data_changed)
{
    uint32_t h = vms_f11b_ser_rd32(vb);
    uint32_t d = vms_f11b_ser_rd32(vb + 4);
    uint32_t i;

    if (h == 0u && d == 0u) {
        h = 1u;
        d = 1u;
    }
    if (hdr_changed) {
        h++;
        if (h == 0u)
            h = 1u;
    }
    if (data_changed) {
        d++;
        if (d == 0u)
            d = 1u;
    }
    vms_f11b_ser_wr32(vb, h);
    vms_f11b_ser_wr32(vb + 4, d);
    for (i = 8u; i < VMS_F11B_SER_VALBLK_LEN; i++)
        vb[i] = 0u;
}

#endif /* VMS_F11B_SER_H */
