/*
 * stat.h - struct stat as musl presents it, Alpha LP64.
 * OVMX alpha-dec-vms musl port (vms-960).
 * Generic LP64 layout (same as aarch64); the kernel-side kstat marshalling is
 * stubbed at rung 1 and reconciled with the OVMX Alpha executive at GAP3.
 */
struct stat {
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
	struct timespec st_atim;
	struct timespec st_mtim;
	struct timespec st_ctim;
	unsigned __unused[2];
};
