/*
 * fcntl.h - open()/fcntl() flags, Alpha ABI.
 * OVMX alpha-dec-vms musl port (vms-960).
 *
 * Values are the public Alpha/Linux ABI (Linux arch/alpha uapi/asm/fcntl.h;
 * glibc's "alpha" bits/fcntl variant). Alpha famously diverges from the generic
 * layout in both O_* (O_NONBLOCK=04, O_APPEND=010, ...) and F_* (F_SETOWN=5,
 * F_GETOWN=6, F_GETLK=7, F_SETLK=8, F_SETLKW=9).
 *
 * RUNG-1 note: the syscall backend is stubbed (-ENOSYS), so these values reach
 * no kernel yet. They are the documented placeholder aligned to the OVMX Alpha
 * runtime lane (OVMX/Linux-Alpha) and will be reconciled with the OVMX Alpha
 * executive's open/fcntl contract at GAP3.
 */
#define O_CREAT        01000
#define O_TRUNC        02000
#define O_EXCL         04000
#define O_NOCTTY      010000
#define O_NONBLOCK        04
#define O_APPEND         010
#define O_DSYNC       040000
#define O_DIRECTORY  0100000
#define O_NOFOLLOW   0200000
#define O_LARGEFILE  0400000
#define O_DIRECT    02000000
#define O_NOATIME   04000000
#define O_CLOEXEC  010000000

#define __O_SYNC   020000000
#define O_SYNC     (__O_SYNC|O_DSYNC)
#define O_RSYNC    O_SYNC

#define O_PATH     040000000
#define __O_TMPFILE 0100000000
#define O_TMPFILE  (__O_TMPFILE|O_DIRECTORY)

#define O_ASYNC       020000
#define O_NDELAY   O_NONBLOCK

#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4

#define F_SETOWN 5
#define F_GETOWN 6
#define F_SETSIG 10
#define F_GETSIG 11

#define F_GETLK  7
#define F_SETLK  8
#define F_SETLKW 9

#define F_SETOWN_EX 15
#define F_GETOWN_EX 16

#define F_GETOWNER_UIDS 17
