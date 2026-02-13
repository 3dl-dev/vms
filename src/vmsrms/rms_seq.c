/*
 * rms_seq.c - RMS Sequential File Organization
 *
 * Implements record-level I/O for sequentially organized files.
 * Supports all VMS record formats:
 *   - STMLF: Stream records delimited by line-feed
 *   - STMCR: Stream records delimited by carriage-return
 *   - STM:   Stream records delimited by CR-LF
 *   - FIX:   Fixed-length records
 *   - VAR:   Variable-length records with 2-byte count prefix
 *   - VFC:   Variable with fixed control area
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rms/rms.h"

/*
 * Helper: Read exactly 'count' bytes from fd at current position.
 * Returns number of bytes read, or -1 on error.
 */
static ssize_t read_exact(int fd, void *buf, size_t count)
{
    size_t total = 0;
    char *p = (char *)buf;

    while (total < count) {
        ssize_t n = read(fd, p + total, count - total);
        if (n < 0) return -1;
        if (n == 0) break;  /* EOF */
        total += (size_t)n;
    }

    return (ssize_t)total;
}

/*
 * Helper: Write exactly 'count' bytes to fd.
 * Returns 0 on success, -1 on error.
 */
static int write_exact(int fd, const void *buf, size_t count)
{
    size_t total = 0;
    const char *p = (const char *)buf;

    while (total < count) {
        ssize_t n = write(fd, p + total, count - total);
        if (n < 0) return -1;
        if (n == 0) return -1;
        total += (size_t)n;
    }

    return 0;
}

/*
 * rms_seq_get - Read a record from a sequential file.
 *
 * Reads the next record starting from the current position
 * tracked in rab->_current_offset. The record is placed into
 * rab->rab$l_ubf and rab->rab$w_rsz is set to the actual
 * record length.
 *
 * For stream formats, the delimiter is consumed but not returned.
 * For VAR format, records are word-aligned (padded to even byte).
 * For VFC format, the fixed control area precedes the data.
 *
 * Returns:
 *   RMS$_NORMAL  - Record read successfully
 *   RMS$_EOF     - End of file reached
 *   RMS$_RTB     - Record too big for user buffer
 *   RMS$_RER     - Read error
 *   RMS$_RAB     - Invalid RAB (no buffer)
 *   RMS$_ACC     - File not accessible
 */
uint32_t rms_seq_get(struct FAB *fab, struct RAB *rab)
{
    int fd = fab->_linux_fd;
    if (fd < 0) return RMS$_ACC;
    if (rab->_eof) return RMS$_EOF;
    if (!rab->rab$l_ubf || rab->rab$w_usz == 0) return RMS$_RAB;

    /* Seek to current stream position */
    lseek(fd, rab->_current_offset, SEEK_SET);

    /* Remember where this record starts */
    rab->_last_rec_offset = rab->_current_offset;

    switch (fab->fab$b_rfm) {
        case FAB$C_STMLF:
        case FAB$C_STM:
        case FAB$C_STMCR: {
            /*
             * Stream formats: read character by character until
             * the appropriate delimiter is found.
             *   STMLF: delimiter is \n (skip \r before \n)
             *   STMCR: delimiter is \r
             *   STM:   delimiter is \r\n pair
             */
            char delim = (fab->fab$b_rfm == FAB$C_STMCR) ? '\r' : '\n';
            int pos = 0;
            char ch;
            ssize_t n;

            while (pos < rab->rab$w_usz) {
                n = read(fd, &ch, 1);
                if (n <= 0) {
                    if (pos == 0) {
                        rab->_eof = 1;
                        return RMS$_EOF;
                    }
                    break;  /* Return partial record at EOF */
                }
                if (ch == delim) break;
                /* For STMLF, skip carriage returns */
                if (ch == '\r' && fab->fab$b_rfm == FAB$C_STMLF) continue;
                /* For STM, \r is part of \r\n delimiter */
                if (ch == '\r' && fab->fab$b_rfm == FAB$C_STM) {
                    /* Peek at next char */
                    char next;
                    ssize_t pn = read(fd, &next, 1);
                    if (pn > 0 && next == '\n') break;  /* \r\n found */
                    /* Not \r\n, include \r in record */
                    if (pos < rab->rab$w_usz) rab->rab$l_ubf[pos++] = '\r';
                    if (pn > 0 && pos < rab->rab$w_usz) {
                        rab->rab$l_ubf[pos++] = next;
                    }
                    continue;
                }
                rab->rab$l_ubf[pos++] = ch;
            }

            rab->rab$w_rsz = (uint16_t)pos;
            rab->_last_rec_size = (uint16_t)pos;
            rab->_current_offset = lseek(fd, 0, SEEK_CUR);
            break;
        }

        case FAB$C_FIX: {
            /*
             * Fixed-length records: read exactly fab$w_mrs bytes.
             */
            uint16_t recsize = fab->fab$w_mrs;
            if (recsize == 0) recsize = 512;

            if (recsize > rab->rab$w_usz) {
                rab->rab$l_stv = recsize;
                return RMS$_RTB;
            }

            ssize_t n = read_exact(fd, rab->rab$l_ubf, recsize);
            if (n <= 0) {
                rab->_eof = 1;
                return RMS$_EOF;
            }
            if (n < recsize) {
                /* Partial record at EOF - pad with spaces per VMS convention */
                memset(rab->rab$l_ubf + n, ' ', (size_t)(recsize - n));
            }

            rab->rab$w_rsz = recsize;
            rab->_last_rec_size = recsize;
            rab->_current_offset += recsize;
            break;
        }

        case FAB$C_VAR: {
            /*
             * Variable-length records: 2-byte little-endian count prefix
             * followed by that many bytes of data. Records are padded
             * to word (2-byte) boundaries.
             */
            uint16_t reclen;
            ssize_t n = read_exact(fd, &reclen, 2);
            if (n <= 0) {
                rab->_eof = 1;
                return RMS$_EOF;
            }
            if (n < 2) return RMS$_RER;

            if (reclen > rab->rab$w_usz) {
                rab->rab$l_stv = reclen;
                /* Skip past the record data so we don't corrupt the stream */
                lseek(fd, reclen + (reclen & 1), SEEK_CUR);
                rab->_current_offset = lseek(fd, 0, SEEK_CUR);
                return RMS$_RTB;
            }

            if (reclen > 0) {
                n = read_exact(fd, rab->rab$l_ubf, reclen);
                if (n < reclen) return RMS$_RER;
            }

            /* Skip pad byte for word alignment */
            if (reclen & 1) lseek(fd, 1, SEEK_CUR);

            rab->rab$w_rsz = reclen;
            rab->_last_rec_size = reclen;
            rab->_current_offset = lseek(fd, 0, SEEK_CUR);
            break;
        }

        case FAB$C_VFC: {
            /*
             * Variable with Fixed Control: 2-byte count prefix,
             * then fab$b_fsz bytes of fixed control area (carriage
             * control etc.), then the variable-length data.
             * The count includes the fixed control area.
             */
            uint16_t reclen;
            ssize_t n = read_exact(fd, &reclen, 2);
            if (n <= 0) {
                rab->_eof = 1;
                return RMS$_EOF;
            }
            if (n < 2) return RMS$_RER;

            uint8_t fsz = fab->fab$b_fsz;
            if (fsz == 0) fsz = 2;  /* Default VFC header size */

            uint16_t datalen = (reclen > fsz) ? (reclen - fsz) : 0;

            if (datalen > rab->rab$w_usz) {
                rab->rab$l_stv = datalen;
                lseek(fd, reclen + (reclen & 1), SEEK_CUR);
                rab->_current_offset = lseek(fd, 0, SEEK_CUR);
                return RMS$_RTB;
            }

            /* Read and discard the fixed control area */
            if (fsz > 0) {
                char ctrl[256];
                uint8_t to_read = (fsz <= sizeof(ctrl)) ? fsz : (uint8_t)sizeof(ctrl);
                n = read_exact(fd, ctrl, to_read);
                if (n < to_read) return RMS$_RER;
            }

            /* Read the data portion */
            if (datalen > 0) {
                n = read_exact(fd, rab->rab$l_ubf, datalen);
                if (n < datalen) return RMS$_RER;
            }

            /* Skip pad byte for word alignment */
            if (reclen & 1) lseek(fd, 1, SEEK_CUR);

            rab->rab$w_rsz = datalen;
            rab->_last_rec_size = datalen;
            rab->_current_offset = lseek(fd, 0, SEEK_CUR);
            break;
        }

        default:
            return RMS$_ORG;
    }

    return RMS$_NORMAL;
}

/*
 * rms_seq_put - Write a record to a sequential file.
 *
 * Writes a record at the end of the file (sequential access).
 * The record data is taken from rab->rab$l_rbf (or rab$l_ubf)
 * and the length from rab->rab$w_rsz.
 *
 * Returns:
 *   RMS$_NORMAL - Record written successfully
 *   RMS$_WER    - Write error
 *   RMS$_RTB    - Record too big (exceeds MRS for FIX)
 *   RMS$_DME    - Dynamic memory exhausted
 *   RMS$_RAB    - Invalid RAB (no buffer)
 *   RMS$_ACC    - File not accessible
 */
uint32_t rms_seq_put(struct FAB *fab, struct RAB *rab)
{
    int fd = fab->_linux_fd;
    if (fd < 0) return RMS$_ACC;

    char *buf = rab->rab$l_rbf ? rab->rab$l_rbf : rab->rab$l_ubf;
    if (!buf) return RMS$_RAB;
    uint16_t len = rab->rab$w_rsz;

    /* Seek to end for sequential writes */
    lseek(fd, 0, SEEK_END);

    switch (fab->fab$b_rfm) {
        case FAB$C_STMLF: {
            /* Stream-LF: write data followed by \n */
            if (len > 0) {
                if (write_exact(fd, buf, len) < 0) return RMS$_WER;
            }
            if (write_exact(fd, "\n", 1) < 0) return RMS$_WER;
            break;
        }

        case FAB$C_STMCR: {
            /* Stream-CR: write data followed by \r */
            if (len > 0) {
                if (write_exact(fd, buf, len) < 0) return RMS$_WER;
            }
            if (write_exact(fd, "\r", 1) < 0) return RMS$_WER;
            break;
        }

        case FAB$C_STM: {
            /* Stream: write data followed by \r\n */
            if (len > 0) {
                if (write_exact(fd, buf, len) < 0) return RMS$_WER;
            }
            if (write_exact(fd, "\r\n", 2) < 0) return RMS$_WER;
            break;
        }

        case FAB$C_FIX: {
            /*
             * Fixed-length: write exactly MRS bytes.
             * If the user's record is shorter, pad with spaces.
             * If longer, truncate.
             */
            uint16_t recsize = fab->fab$w_mrs;
            if (recsize == 0) recsize = len;

            if (len > recsize && fab->fab$w_mrs > 0) {
                rab->rab$l_stv = len;
                return RMS$_RTB;
            }

            char *padded = calloc(1, recsize);
            if (!padded) return RMS$_DME;

            /* Pad with spaces (VMS convention for text records) */
            memset(padded, ' ', recsize);
            uint16_t copylen = (len < recsize) ? len : recsize;
            memcpy(padded, buf, copylen);

            int rc = write_exact(fd, padded, recsize);
            free(padded);
            if (rc < 0) return RMS$_WER;
            break;
        }

        case FAB$C_VAR: {
            /*
             * Variable-length: write 2-byte count prefix followed
             * by the record data. Pad to word boundary.
             */
            if (fab->fab$w_mrs > 0 && len > fab->fab$w_mrs) {
                rab->rab$l_stv = len;
                return RMS$_RTB;
            }

            if (write_exact(fd, &len, 2) < 0) return RMS$_WER;
            if (len > 0) {
                if (write_exact(fd, buf, len) < 0) return RMS$_WER;
            }
            /* Pad to word boundary */
            if (len & 1) {
                char zero = 0;
                if (write_exact(fd, &zero, 1) < 0) return RMS$_WER;
            }
            break;
        }

        case FAB$C_VFC: {
            /*
             * Variable with Fixed Control: write 2-byte total length
             * (including fixed control area), then the fixed control
             * bytes (zeroed if not specified), then the data.
             */
            uint8_t fsz = fab->fab$b_fsz;
            if (fsz == 0) fsz = 2;

            if (fab->fab$w_mrs > 0 && len > fab->fab$w_mrs) {
                rab->rab$l_stv = len;
                return RMS$_RTB;
            }

            uint16_t total = (uint16_t)(fsz + len);
            if (write_exact(fd, &total, 2) < 0) return RMS$_WER;

            /* Write fixed control area (zeroed) */
            char ctrl[256];
            memset(ctrl, 0, sizeof(ctrl));
            if (write_exact(fd, ctrl, fsz) < 0) return RMS$_WER;

            /* Write data */
            if (len > 0) {
                if (write_exact(fd, buf, len) < 0) return RMS$_WER;
            }

            /* Pad to word boundary */
            if (total & 1) {
                char zero = 0;
                if (write_exact(fd, &zero, 1) < 0) return RMS$_WER;
            }
            break;
        }

        default:
            return RMS$_ORG;
    }

    rab->_current_offset = lseek(fd, 0, SEEK_CUR);
    return RMS$_NORMAL;
}
