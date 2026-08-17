/*
 * rms_io.h - RMS block-I/O substrate over the Files-11 (ODS-2) ACP (vms-bc7,
 * epic vms-208).
 *
 * THE SUBSTRATE SWAP. RMS used to reach file data through a per-process Linux
 * file descriptor (FAB._linux_fd) with positioned POSIX I/O (lseek/read/write).
 * That is gone. RMS now reaches file data the VMS way: a channel $ASSIGNed to
 * the mounted ODS-2 volume, a file ACCESSed on that channel (IO$_ACCESS /
 * IO$_CREATE builds the VBN->LBN retrieval-pointer window), and virtual-block
 * transfers (IO$_READVBLK / IO$_WRITEVBLK) through that window -- all in the
 * executive, over /dev/vms (src/libvmssys/vms_kif.c acp wrappers).
 *
 * The seq/rel/idx record engines are UNCHANGED in their record logic: they
 * still think in file byte offsets (rab->_current_offset et al.) and still call
 * a small "positioned I/O" vocabulary. This header re-homes exactly that
 * vocabulary from a bare `int fd` onto an `rms_file_t *` handle whose backing is
 * the ACP channel+window. rms_io_lseek/read/write/read_exact/write_exact
 * reproduce the POSIX fd cursor semantics (a byte cursor the engines set with
 * lseek and advance with read/write) on top of {VBN, byte-offset, length}
 * READVBLK/WRITEVBLK. This is the design's "positioned-I/O sub-project, done the
 * VMS way (virtual-block QIO)" (docs/design-files11-acp-executive.md §4.5).
 *
 * FAIL-HONEST (CLAUDE.md Rule 9 / INV-6). Every primitive rides
 * vms_kif_acp_*, which returns SS$_NOSUCHDEV when /dev/vms is absent and the
 * real SS$_ status otherwise -- there is NO POSIX fallback. A read/write on a
 * handle with no accessed file, or against an absent executive, fails; it never
 * silently succeeds against a private substitute.
 */
#ifndef RMS_IO_H
#define RMS_IO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * An RMS accessed file: the executive channel $ASSIGNed to the volume plus the
 * state that lets the record engines treat it like a POSIX fd. Created by
 * rms_core (rms_impl_open / rms_impl_create) via the ACP, stored on
 * FAB._rms_file, and torn down by rms_impl_close. The record engines only ever
 * read `cursor`/`eof` indirectly through the rms_io_* primitives below.
 */
typedef struct rms_file {
    uint32_t chan;        /* executive channel ($ASSIGN of the mounted volume) */
    int      assigned;    /* 1 once the channel is $ASSIGNed                    */
    int      accessed;    /* 1 once IO$_ACCESS / IO$_CREATE built a window      */
    int      writable;    /* 1 if accessed for write                           */
    uint64_t cursor;      /* byte cursor (emulates the POSIX fd position)       */
    uint64_t eof;         /* valid byte length (efblk/ffbyte-derived)          */
    uint32_t hiblk;       /* highest allocated VBN                             */
    /* FID of the accessed file (IO$_DELETE by-FID, diagnostics). */
    uint16_t fid_num;
    uint16_t fid_seq;
    uint8_t  fid_rvn;
    uint8_t  fid_nmx;
    /* POSIX backend (non-__linux__ / netbsd-vax standalone cross) only: the
     * bare fd the handle now owns in place of the retired FAB._linux_fd. Unused
     * by the ACP backend. */
    int      fd;
} rms_file_t;

/*
 * Positioned-I/O vocabulary. Each mirrors the POSIX call the record engines
 * used to make on the bare fd, but translates the byte cursor into
 * {VBN, byte-offset, length} transfers on the ACP channel window.
 */

/* lseek(2) emulation: move the byte cursor. whence is SEEK_SET/CUR/END; END is
 * relative to the file's valid byte length (f->eof). Returns the new absolute
 * byte position, or (off_t)-1 on a bad handle. */
off_t rms_io_lseek(rms_file_t *f, off_t offset, int whence);

/* read(2) emulation: read up to `n` bytes at the cursor via IO$_READVBLK,
 * advance the cursor by the count returned. Returns the byte count (0 at EOF),
 * or -1 on error (an executive/channel failure, never a POSIX fallback). */
ssize_t rms_io_read(rms_file_t *f, void *buf, size_t n);

/* write(2) emulation: write `n` bytes at the cursor via IO$_WRITEVBLK (which
 * implicitly extends past EOF), advance the cursor, grow f->eof. Returns the
 * byte count written, or -1 on error. */
ssize_t rms_io_write(rms_file_t *f, const void *buf, size_t n);

/* Read exactly `count` bytes (short only at EOF). Mirrors the old
 * rms_read_exact(fd,...). Returns bytes read (< count only at EOF) or -1. */
ssize_t rms_io_read_exact(rms_file_t *f, void *buf, size_t count);

/* Write exactly `count` bytes. Mirrors the old rms_write_exact(fd,...).
 * Returns 0 on success, -1 on error. */
int rms_io_write_exact(rms_file_t *f, const void *buf, size_t count);

/* ftruncate(2) emulation: set the file's valid byte length to `length`.
 * Growing pre-allocates+zero-fills through the ACP (implicit extend); shrinking
 * issues IO$_MODIFY truncate. Returns 0 on success, -1 on error. Used for
 * relative-file cell pre-allocation. */
int rms_io_ftruncate(rms_file_t *f, off_t length);

/* fsync(2) emulation: IO$_WRITEVBLK is write-through to the block device
 * (the ACP writes each block synchronously), so there is nothing buffered to
 * flush. Returns 0. Present so the record engines keep a symmetric vocabulary. */
int rms_io_fsync(rms_file_t *f);

#if !defined(__linux__)
/* POSIX-backend (netbsd-vax standalone cross) only: wrap a freshly open(2)'d fd
 * in a handle (eof seeded from the file's current size), release it (close +
 * free), and read back the fd. The __linux__ ACP build has no fd -- the FAB
 * lifecycle there $ASSIGNs/IO$_ACCESSes instead. */
rms_file_t *rms_io_posix_wrap(int fd);
void        rms_io_posix_unwrap(rms_file_t *f);   /* close(fd) + free(handle) */
int         rms_io_posix_fd(rms_file_t *f);
#endif

#endif /* RMS_IO_H */
