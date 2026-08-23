/*
 * kstat.h - kernel stat buffer layout for the Alpha syscall backend.
 * OVMX alpha-dec-vms musl port (vms-960).
 *
 * RUNG-1 note: this is the LP64 kstat layout musl expects to marshal to/from
 * the kernel's fstatat/statx path. The syscall backend is stubbed (-ENOSYS) at
 * rung 1, so this struct is not yet exchanged with any real kernel; the exact
 * field layout will be reconciled with the OVMX Alpha executive stat contract
 * at GAP3. Kept identical to the generic LP64 (aarch64) kstat for now.
 */
struct kstat {
	dev_t st_dev;
	ino_t st_ino;
	mode_t st_mode;
	nlink_t st_nlink;
	uid_t st_uid;
	gid_t st_gid;
	dev_t st_rdev;
	unsigned long __pad;
	off_t st_size;
	blksize_t st_blksize;
	int __pad2;
	blkcnt_t st_blocks;
	long st_atime_sec;
	long st_atime_nsec;
	long st_mtime_sec;
	long st_mtime_nsec;
	long st_ctime_sec;
	long st_ctime_nsec;
	unsigned __unused[2];
};
