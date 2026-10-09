/* musl 1.2.4 arch/x86_64/kstat.h (MIT): the buffer SYS_fstat / SYS_newfstatat fill.
 * The C RTL file layer over RMS (src/vmsrms/crtl_rms_fd.c) fills it for an RMS file. */
#include <sys/types.h>
struct kstat {
	dev_t st_dev;
	ino_t st_ino;
	nlink_t st_nlink;

	mode_t st_mode;
	uid_t st_uid;
	gid_t st_gid;
	unsigned int    __pad0;
	dev_t st_rdev;
	off_t st_size;
	blksize_t st_blksize;
	blkcnt_t st_blocks;

	long st_atime_sec;
	long st_atime_nsec;
	long st_mtime_sec;
	long st_mtime_nsec;
	long st_ctime_sec;
	long st_ctime_nsec;
	long __unused[3];
};
