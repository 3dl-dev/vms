/*
 * rms_rel.c - RMS Relative File Organization
 *
 * Records are stored in fixed-size cells addressable by relative
 * record number (RRN). Each cell consists of:
 *   - 1 byte status (active, deleted, or empty)
 *   - fab$w_mrs bytes of record data
 *
 * Cell size = fab$w_mrs + 1
 *
 * Records can be accessed sequentially (skipping deleted/empty cells)
 * or by key (direct access by record number).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rms/rms.h"

/* Cell status byte values */
#define REL_CELL_EMPTY   0x00    /* Cell has never been used */
#define REL_CELL_ACTIVE  0x01    /* Cell contains a live record */
#define REL_CELL_DELETED 0x02    /* Cell was deleted */

/*
 * Helper: compute the cell size for a relative file.
 * Cell = 1-byte status + record data (MRS bytes).
 */
static size_t cell_size(struct FAB *fab)
{
    return (size_t)fab->fab$w_mrs + 1;
}

/*
 * Helper: Read exactly 'count' bytes.
 */
static ssize_t rel_read_exact(int fd, void *buf, size_t count)
{
    size_t total = 0;
    char *p = (char *)buf;
    while (total < count) {
        ssize_t n = read(fd, p + total, count - total);
        if (n < 0) return -1;
        if (n == 0) break;
        total += (size_t)n;
    }
    return (ssize_t)total;
}

/*
 * Helper: Write exactly 'count' bytes.
 */
static int rel_write_exact(int fd, const void *buf, size_t count)
{
    size_t total = 0;
    const char *p = (const char *)buf;
    while (total < count) {
        ssize_t n = write(fd, p + total, count - total);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

/*
 * rms_rel_get - Read a record from a relative file.
 *
 * Access modes:
 *   RAB$C_KEY: Direct access by record number (rab$l_bkt).
 *              Seeks to cell = bkt * cell_size, reads status byte.
 *              If not active, returns RMS$_RNF.
 *
 *   RAB$C_SEQ: Sequential access. Advances from current position,
 *              skipping deleted and empty cells, until an active
 *              record is found or EOF is reached.
 *
 * Returns:
 *   RMS$_NORMAL - Record read successfully
 *   RMS$_RNF    - Record not found (empty/deleted cell)
 *   RMS$_EOF    - End of file (no more records)
 *   RMS$_RTB    - Record too big for user buffer
 *   RMS$_RER    - Read error
 *   RMS$_RAB    - Invalid RAB
 *   RMS$_ACC    - File not accessible
 */
uint32_t rms_rel_get(struct FAB *fab, struct RAB *rab)
{
    int fd = fab->_linux_fd;
    if (fd < 0) return RMS$_ACC;
    if (!rab->rab$l_ubf || rab->rab$w_usz == 0) return RMS$_RAB;

    size_t csize = cell_size(fab);
    uint32_t rrn;

    if (rab->rab$b_rac == RAB$C_KEY) {
        /* Direct access by record number */
        rrn = rab->rab$l_bkt;
    } else {
        /* Sequential: compute RRN from current offset */
        rrn = (uint32_t)(rab->_current_offset / (off_t)csize);
    }

    /* Check maximum record number */
    if (fab->fab$l_mrn > 0 && rrn >= fab->fab$l_mrn) {
        return RMS$_EOF;
    }

    /* Read loop: for sequential access, skip deleted/empty cells */
    for (;;) {
        off_t offset = (off_t)(rrn * csize);
        if (lseek(fd, offset, SEEK_SET) < 0) {
            return RMS$_EOF;
        }

        uint8_t status_byte;
        ssize_t n = read(fd, &status_byte, 1);
        if (n <= 0) {
            return RMS$_EOF;
        }

        if (status_byte == REL_CELL_ACTIVE) {
            /* Active record found - read the data */
            uint16_t reclen = fab->fab$w_mrs;
            if (reclen > rab->rab$w_usz) {
                rab->rab$l_stv = reclen;
                return RMS$_RTB;
            }

            n = rel_read_exact(fd, rab->rab$l_ubf, reclen);
            if (n < reclen) return RMS$_RER;

            rab->rab$w_rsz = reclen;
            rab->rab$l_bkt = rrn;
            rab->_last_rec_offset = offset;
            rab->_last_rec_size = reclen;
            rab->_current_offset = (off_t)((rrn + 1) * csize);
            return RMS$_NORMAL;
        }

        if (rab->rab$b_rac == RAB$C_KEY) {
            /* Direct access: cell is not active */
            if (status_byte == REL_CELL_DELETED) {
                return RMS$_DEL;
            }
            return RMS$_RNF;
        }

        /* Sequential: advance to next cell */
        rrn++;
        if (fab->fab$l_mrn > 0 && rrn >= fab->fab$l_mrn) {
            return RMS$_EOF;
        }
    }
}

/*
 * rms_rel_put - Write a record to a relative file.
 *
 * If rab$l_bkt is set and access mode is RAB$C_KEY, writes to
 * the specified cell. The cell must be empty or deleted.
 *
 * For sequential access, finds the next empty cell or appends.
 *
 * Returns:
 *   RMS$_NORMAL - Record written successfully
 *   RMS$_REX    - Record already exists in that cell
 *   RMS$_MRN    - Record number exceeds maximum
 *   RMS$_RTB    - Record too big
 *   RMS$_WER    - Write error
 *   RMS$_DME    - Memory allocation error
 */
uint32_t rms_rel_put(struct FAB *fab, struct RAB *rab)
{
    int fd = fab->_linux_fd;
    if (fd < 0) return RMS$_ACC;

    char *buf = rab->rab$l_rbf ? rab->rab$l_rbf : rab->rab$l_ubf;
    if (!buf) return RMS$_RAB;

    size_t csize = cell_size(fab);
    uint32_t rrn;

    if (rab->rab$b_rac == RAB$C_KEY) {
        /* Direct placement by record number */
        rrn = rab->rab$l_bkt;

        /* Check MRN */
        if (fab->fab$l_mrn > 0 && rrn >= fab->fab$l_mrn) {
            return RMS$_MRN;
        }

        off_t offset = (off_t)(rrn * csize);
        lseek(fd, offset, SEEK_SET);

        /* Check if cell is already active */
        uint8_t existing;
        ssize_t n = read(fd, &existing, 1);
        if (n == 1 && existing == REL_CELL_ACTIVE) {
            /* Cell already occupied - check UIF (update-if-existent) */
            if (rab->rab$l_rop & RAB$M_UIF) {
                /* Fall through to write */
            } else {
                return RMS$_REX;
            }
        }
    } else {
        /* Sequential: find next empty or deleted cell */
        rrn = (uint32_t)(rab->_current_offset / (off_t)csize);

        for (;;) {
            if (fab->fab$l_mrn > 0 && rrn >= fab->fab$l_mrn) {
                return RMS$_MRN;
            }

            off_t offset = (off_t)(rrn * csize);
            if (lseek(fd, offset, SEEK_SET) < 0) break;

            uint8_t status_byte;
            ssize_t n = read(fd, &status_byte, 1);
            if (n <= 0 || status_byte != REL_CELL_ACTIVE) {
                break;  /* Found an empty or deleted cell */
            }
            rrn++;
        }
    }

    /* Write the record at the determined position */
    off_t offset = (off_t)(rrn * csize);
    lseek(fd, offset, SEEK_SET);

    /* Write status byte */
    uint8_t status_byte = REL_CELL_ACTIVE;
    if (rel_write_exact(fd, &status_byte, 1) < 0) return RMS$_WER;

    /* Write record data, padded to MRS */
    uint16_t reclen = rab->rab$w_rsz;
    if (reclen > fab->fab$w_mrs) {
        return RMS$_RTB;
    }

    char *cell = calloc(1, fab->fab$w_mrs);
    if (!cell) return RMS$_DME;

    memcpy(cell, buf, reclen);
    /* Remaining bytes stay zero (calloc) */

    int rc = rel_write_exact(fd, cell, fab->fab$w_mrs);
    free(cell);
    if (rc < 0) return RMS$_WER;

    rab->rab$l_bkt = rrn;
    rab->_last_rec_offset = offset;
    rab->_last_rec_size = reclen;
    rab->_current_offset = (off_t)((rrn + 1) * csize);

    return RMS$_NORMAL;
}

/*
 * rms_rel_update - Update the current record in a relative file.
 *
 * Rewrites the record at the position of the last successfully
 * accessed record (from $GET or $FIND). The cell must still be active.
 *
 * Returns:
 *   RMS$_NORMAL - Record updated
 *   RMS$_CUR    - No current record
 *   RMS$_RTB    - Record too big
 *   RMS$_WER    - Write error
 */
uint32_t rms_rel_update(struct FAB *fab, struct RAB *rab)
{
    int fd = fab->_linux_fd;
    if (fd < 0) return RMS$_ACC;

    char *buf = rab->rab$l_rbf ? rab->rab$l_rbf : rab->rab$l_ubf;
    if (!buf) return RMS$_RAB;

    /* Must have a current record from a prior $GET/$FIND */
    if (rab->_last_rec_offset == 0 && rab->rab$l_bkt == 0) {
        return RMS$_CUR;
    }

    size_t csize = cell_size(fab);
    off_t offset = (off_t)(rab->rab$l_bkt * csize);
    lseek(fd, offset, SEEK_SET);

    /* Verify cell is still active */
    uint8_t status_byte;
    if (read(fd, &status_byte, 1) != 1 || status_byte != REL_CELL_ACTIVE) {
        return RMS$_CUR;
    }

    /* Write the updated data */
    uint16_t reclen = rab->rab$w_rsz;
    if (reclen > fab->fab$w_mrs) {
        return RMS$_RTB;
    }

    char *cell = calloc(1, fab->fab$w_mrs);
    if (!cell) return RMS$_DME;

    memcpy(cell, buf, reclen);

    int rc = rel_write_exact(fd, cell, fab->fab$w_mrs);
    free(cell);
    if (rc < 0) return RMS$_WER;

    return RMS$_NORMAL;
}

/*
 * rms_rel_delete - Delete the current record from a relative file.
 *
 * Marks the cell's status byte as DELETED. The cell space is
 * retained and can be reused by a subsequent $PUT.
 *
 * Returns:
 *   RMS$_NORMAL - Record deleted
 *   RMS$_CUR    - No current record
 *   RMS$_WER    - Write error
 */
uint32_t rms_rel_delete(struct FAB *fab, struct RAB *rab)
{
    int fd = fab->_linux_fd;
    if (fd < 0) return RMS$_ACC;

    /* Must have a current record */
    if (rab->_last_rec_offset == 0 && rab->rab$l_bkt == 0) {
        return RMS$_CUR;
    }

    size_t csize = cell_size(fab);
    off_t offset = (off_t)(rab->rab$l_bkt * csize);
    lseek(fd, offset, SEEK_SET);

    /* Mark cell as deleted */
    uint8_t status_byte = REL_CELL_DELETED;
    if (rel_write_exact(fd, &status_byte, 1) < 0) return RMS$_WER;

    return RMS$_NORMAL;
}

/*
 * rms_rel_find - Position to a record without reading it.
 *
 * Validates that the specified cell exists and is active,
 * but does not transfer data to the user buffer.
 *
 * Returns:
 *   RMS$_NORMAL - Positioned successfully
 *   RMS$_RNF    - Record not found
 *   RMS$_DEL    - Record was deleted
 *   RMS$_EOF    - Past end of file
 */
uint32_t rms_rel_find(struct FAB *fab, struct RAB *rab)
{
    int fd = fab->_linux_fd;
    if (fd < 0) return RMS$_ACC;

    size_t csize = cell_size(fab);
    uint32_t rrn;

    if (rab->rab$b_rac == RAB$C_KEY) {
        rrn = rab->rab$l_bkt;
    } else {
        rrn = (uint32_t)(rab->_current_offset / (off_t)csize);
    }

    if (fab->fab$l_mrn > 0 && rrn >= fab->fab$l_mrn) {
        return RMS$_EOF;
    }

    off_t offset = (off_t)(rrn * csize);
    lseek(fd, offset, SEEK_SET);

    uint8_t status_byte;
    ssize_t n = read(fd, &status_byte, 1);
    if (n <= 0) return RMS$_EOF;

    if (status_byte == REL_CELL_DELETED) return RMS$_DEL;
    if (status_byte != REL_CELL_ACTIVE) return RMS$_RNF;

    rab->rab$l_bkt = rrn;
    rab->_last_rec_offset = offset;
    rab->_current_offset = (off_t)((rrn + 1) * csize);

    return RMS$_NORMAL;
}
