/*
 * stat.h - struct stat as musl presents it, Alpha LP64.
 * OVMX alpha-dec-vms musl port (vms-960).
 *
 * TWO LAYOUTS (vms-28d part 2). A DEC C client gets the DEC C RTL's struct
 * stat by default: st_ino is the three-word File ID and the st_fab_* members
 * carry the file's RMS record attributes, so VMS-host code such as GCC's libcpp
 * (STAT_SIZE_RELIABLE tests st_fab_rfm against FAB$C_VAR) and incpath
 * (INO_T_EQ compares the File ID words) compiles and behaves as on OpenVMS.
 * Defining _USE_STD_STAT selects the X/Open layout instead, as the DEC C RTL
 * does; the C RTL's own build (__OVMX_LIBC_BUILD) always uses it.
 *
 * The DEC C layout's stat/fstat/lstat/fstatat are DECC$SHR entries that fill it
 * (src/vmslink/ovmx_decc_stat.c); the field list is one macro so the client
 * declaration and the RTL's copy of it cannot drift apart.
 */
#define __OVMX_DECC_STAT_FIELDS \
	dev_t st_dev; \
	ino_t st_ino[3];          /* File ID: number(+NMX), sequence, RVN */ \
	mode_t st_mode; \
	nlink_t st_nlink; \
	uid_t st_uid; \
	gid_t st_gid; \
	dev_t st_rdev; \
	off_t st_size; \
	struct timespec st_atim; \
	struct timespec st_mtim; \
	struct timespec st_ctim; \
	char st_fab_rfm;          /* record format (FAB$C_*) */ \
	char st_fab_rat;          /* record attributes (FAB$M_*) */ \
	char st_fab_fsz;          /* fixed-control area size (VFC) */ \
	unsigned st_fab_mrs;      /* maximum record size */ \
	blksize_t st_blksize; \
	blkcnt_t st_blocks;

#if defined(__VMS) && !defined(__OVMX_LIBC_BUILD) && !defined(_USE_STD_STAT)
#define __OVMX_DECC_STAT 1
struct stat {
	__OVMX_DECC_STAT_FIELDS
};
/* Bound here, before <sys/stat.h> declares them, so the label sticks. */
int stat(const char *__restrict, struct stat *__restrict) __asm__("decc$$stat");
int fstat(int, struct stat *) __asm__("decc$$fstat");
int lstat(const char *__restrict, struct stat *__restrict) __asm__("decc$$lstat");
int fstatat(int, const char *__restrict, struct stat *__restrict, int) __asm__("decc$$fstatat");
#else
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
#endif
