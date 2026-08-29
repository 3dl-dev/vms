/*
 * imgact_acp.c - IMGACT image reads over the executive Files-11 (ODS-2) ACP.
 *
 * See imgact_acp.h for the model and the no-POSIX-fallback (INV-6) contract.
 *
 * This translation unit is compiled BOTH into the freestanding IMGACT.EXE (as
 * an extra source, exactly like known_images.c) and into the hosted QEMU test
 * (tests/qemu/test_syssvc_imgact_acp.c). It therefore uses only its own static
 * memory helpers and the three host primitives declared in imgact_acp.h -- no
 * libc, no imgact.c internals -- so the same code runs in both worlds.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8). The ACP ioctl arg structs / request numbers
 * are OVMX design choices labelled as such in src/kernel/vms_acp.h; this file
 * only marshals into them. It reads no OpenVMS byte-level wire.
 */

#include <stdint.h>

/* One include pulls in the _IOWR encoding, VMS_IOC_MAGIC, VMS_DEVNAM_SIZE, and
 * every ACP / register / dassgn arg struct + request number (vms_ioctl.h
 * includes vms_lnm.h, vms_mbx.h and vms_acp.h at its foot). */
#include "vms_ioctl.h"
#include "ssdef.h"

#include "imgact_acp.h"

/* --------------------------------------------------------------------------
 * Local, self-contained helpers (no libc; usable in the freestanding link).
 * -------------------------------------------------------------------------- */

static void acp_memset(void *d, int c, unsigned long n)
{
	unsigned char *p = d;
	while (n--)
		*p++ = (unsigned char)c;
}

static unsigned long acp_strlen(const char *s)
{
	const char *p = s;
	while (*p)
		p++;
	return (unsigned long)(p - s);
}

/* Copy at most cap-1 bytes of NUL-terminated src into dst; always NUL-term. */
static void acp_strlcpy(char *dst, const char *src, unsigned long cap)
{
	unsigned long i = 0;
	if (cap == 0)
		return;
	for (; i + 1 < cap && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

/* --------------------------------------------------------------------------
 * ACP primitives -- one thin wrapper per ioctl the activator needs.
 * -------------------------------------------------------------------------- */

/* Register (adopt-or-create) this process's executive PCB. The executive keys
 * the PCB on the thread group and it survives execve, so when IMGACT runs as
 * the interpreter of an image DCL activated (register-continue), this ADOPTS
 * the existing row (vms_module.c: "register a process that already exists ->
 * hand back the process that already exists"); in a fresh process (the test)
 * it creates one. Either way the ACP ioctls that follow have a process. */
static uint32_t acp_register(int fd)
{
	struct vms_register_args a;
	acp_memset(&a, 0, sizeof(a));
	if (imgact_acp_dev_ioctl(fd, VMS_IOCTL_REGISTER, &a) < 0)
		return SS$_NOSUCHDEV;
	return a.status;
}

/* $ASSIGN a FILE-CLASS channel to the mounted ODS-2 volume via the DEDICATED
 * ACP assign ioctl (VMS_IOCTL_ACP_ASSIGN, 0x6A) -- the same one vms_kif_acp_
 * assign() issues. The generic VMS_IOCTL_ASSIGN (0x50) is NOT equivalent: the
 * "$ASSIGN of a mounted volume routes to the ACP" behaviour lives in userspace
 * sys_assign.c, which this freestanding path does not run, so it must name the
 * ACP assign directly or the channel is not file-class and IO$_ACCESS on it
 * fails. SS$_NOSUCHDEV when the unit is not an ACP-mounted volume (fail-honest). */
static uint32_t acp_assign(int fd, const char *dev, uint32_t *chan)
{
	struct vms_acp_assign_args a;
	acp_memset(&a, 0, sizeof(a));
	acp_strlcpy(a.devnam, dev, sizeof(a.devnam));
	if (imgact_acp_dev_ioctl(fd, VMS_IOCTL_ACP_ASSIGN, &a) < 0)
		return SS$_NOSUCHDEV;
	if ($VMS_STATUS_SUCCESS(a.status))
		*chan = a.chan;
	return a.status;
}

static void acp_dassgn(int fd, uint32_t chan)
{
	struct vms_dassgn_args a;
	acp_memset(&a, 0, sizeof(a));
	a.chan = chan;
	(void)imgact_acp_dev_ioctl(fd, VMS_IOCTL_DASSGN, &a);
}

static void acp_deaccess(int fd, uint32_t chan)
{
	struct vms_acp_deaccess_args a;
	acp_memset(&a, 0, sizeof(a));
	a.chan = chan;
	(void)imgact_acp_dev_ioctl(fd, VMS_IOCTL_ACP_DEACCESS, &a);
}

/*
 * IO$_ACCESS one path component on `chan`. `name` is "NAME.TYPE" (a directory
 * is "NAME.DIR"); `did_*` is the directory to search (0,0,0 => the MFD). On
 * success returns SS$_NORMAL and fills *out_fid_* + the file geometry
 * (*out_valid = total valid bytes; 0 for a directory, which the caller does
 * not read). Leaves the file accessed on the channel (the caller deaccesses).
 */
static uint32_t acp_access_name(int fd, uint32_t chan, const char *name,
				uint16_t did_num, uint16_t did_seq,
				uint8_t did_rvn, uint8_t did_nmx,
				uint16_t *out_fid_num, uint16_t *out_fid_seq,
				uint8_t *out_fid_rvn, uint8_t *out_fid_nmx,
				uint32_t *out_valid)
{
	struct vms_acp_access_args a;

	acp_memset(&a, 0, sizeof(a));
	a.chan    = chan;
	a.did_num = did_num;
	a.did_seq = did_seq;
	a.did_rvn = did_rvn;
	a.did_nmx = did_nmx;
	acp_strlcpy(a.name, name, sizeof(a.name));

	if (imgact_acp_dev_ioctl(fd, VMS_IOCTL_ACP_ACCESS, &a) < 0)
		return SS$_NOSUCHDEV;

	if ($VMS_STATUS_SUCCESS(a.status)) {
		if (out_fid_num) *out_fid_num = a.fid_num;
		if (out_fid_seq) *out_fid_seq = a.fid_seq;
		if (out_fid_rvn) *out_fid_rvn = a.fid_rvn;
		if (out_fid_nmx) *out_fid_nmx = a.fid_nmx;
		if (out_valid) {
			uint32_t efblk = a.attr.efblk;
			*out_valid = efblk
				? (efblk - 1u) * 512u + a.attr.ffbyte
				: 0u;
		}
	}
	return a.status;
}

/* --------------------------------------------------------------------------
 * Path parsing: pull the next '/'-separated component out of *pp into buf,
 * advancing *pp. Returns the component length, or 0 when the path is
 * exhausted. A leading "/vms" (the retired POSIX mount point) and empty
 * components (leading/duplicate slashes) are skipped by the caller loop.
 * -------------------------------------------------------------------------- */
static unsigned long next_component(const char **pp, char *buf, unsigned long cap)
{
	const char *p = *pp;
	unsigned long n = 0;

	while (*p == '/')
		p++;
	while (*p && *p != '/') {
		if (n + 1 < cap)
			buf[n] = *p;
		n++;
		p++;
	}
	buf[n < cap ? n : cap - 1] = '\0';
	*pp = p;
	return n;
}

/* --------------------------------------------------------------------------
 * Public API.
 * -------------------------------------------------------------------------- */

uint32_t imgact_acp_open(struct imgact_acp_file *f, const char *dev,
			 const char *path)
{
	uint32_t st;
	int fd;
	const char *p = path;
	char comp[VMS_ACP_NAME_SIZE];
	char next[VMS_ACP_NAME_SIZE];
	uint16_t did_num = 0, did_seq = 0;
	uint8_t  did_rvn = 0, did_nmx = 0;

	acp_memset(f, 0, sizeof(*f));
	f->dev_fd = -1;

	fd = imgact_acp_dev_open();
	if (fd < 0)
		return SS$_NOSUCHDEV;
	f->dev_fd = fd;

	st = acp_register(fd);
	if (!$VMS_STATUS_SUCCESS(st)) {
		imgact_acp_dev_close(fd);
		f->dev_fd = -1;
		return st;
	}

	st = acp_assign(fd, dev, &f->chan);
	if (!$VMS_STATUS_SUCCESS(st)) {
		imgact_acp_dev_close(fd);
		f->dev_fd = -1;
		return st;
	}

	/* Strip a leading "/vms" mount-point component if present. */
	{
		const char *q = p;
		while (*q == '/')
			q++;
		if ((q[0] == 'v' || q[0] == 'V') &&
		    (q[1] == 'm' || q[1] == 'M') &&
		    (q[2] == 's' || q[2] == 'S') &&
		    (q[3] == '/' || q[3] == '\0'))
			p = q + 3;
	}

	/* Pull the first real component; each subsequent one is looked ahead so
	 * the LAST component is opened as a file and the rest as directories. */
	if (next_component(&p, comp, sizeof(comp)) == 0) {
		acp_dassgn(fd, f->chan);
		imgact_acp_dev_close(fd);
		f->dev_fd = -1;
		return SS$_NOSUCHFILE;
	}

	for (;;) {
		int has_next = (next_component(&p, next, sizeof(next)) != 0);

		if (!has_next) {
			/* Final component: the image file itself. Leave it
			 * accessed on the channel for imgact_acp_pread(). */
			st = acp_access_name(fd, f->chan, comp,
					     did_num, did_seq, did_rvn, did_nmx,
					     0, 0, 0, 0, &f->valid);
			if (!$VMS_STATUS_SUCCESS(st)) {
				acp_dassgn(fd, f->chan);
				imgact_acp_dev_close(fd);
				f->dev_fd = -1;
				return st;
			}
			f->accessed = 1;
			return st;
		}

		/* Intermediate component: an ODS-2 directory "NAME.DIR". Resolve
		 * its FID, then DID-chain to it for the next level. We only need
		 * the FID, so release the directory before descending. */
		{
			char dirname[VMS_ACP_NAME_SIZE];
			uint16_t fnum = 0, fseq = 0;
			uint8_t  frvn = 0, fnmx = 0;
			unsigned long L = acp_strlen(comp);

			acp_strlcpy(dirname, comp, sizeof(dirname));
			if (L + 4 < sizeof(dirname)) {
				dirname[L + 0] = '.';
				dirname[L + 1] = 'D';
				dirname[L + 2] = 'I';
				dirname[L + 3] = 'R';
				dirname[L + 4] = '\0';
			}

			st = acp_access_name(fd, f->chan, dirname,
					     did_num, did_seq, did_rvn, did_nmx,
					     &fnum, &fseq, &frvn, &fnmx, 0);
			if (!$VMS_STATUS_SUCCESS(st)) {
				acp_dassgn(fd, f->chan);
				imgact_acp_dev_close(fd);
				f->dev_fd = -1;
				return st;
			}
			acp_deaccess(fd, f->chan);

			did_num = fnum;
			did_seq = fseq;
			did_rvn = frvn;
			did_nmx = fnmx;
		}

		/* Advance: the looked-ahead component becomes the current one. */
		acp_strlcpy(comp, next, sizeof(comp));
	}
}

/*
 * The executive caps a single IO$_READVBLK at ACP_RW_MAX_XFER (1 MiB) and
 * rejects a longer length with SS$_BADPARAM (src/kernel-core/vmsfs_acp.c:1878
 * and its check at ~:1972). That per-QIO bound is legitimate and stays; a
 * caller reading more than 1 MiB in one logical read must chunk. Mirror the
 * constant here (as rms_io.c does with RMS_IO_MAX_XFER) rather than reach into
 * kernel-core, which is not on imgact's include path.
 */
#define IMGACT_ACP_RW_MAX_XFER (1u << 20)  /* mirror ACP_RW_MAX_XFER (vmsfs_acp.c:1878) */

long imgact_acp_pread(struct imgact_acp_file *f, void *buf,
		      unsigned long n, long off)
{
	unsigned long avail;
	unsigned long remaining;
	unsigned long cur_off;
	unsigned long xferred = 0;
	uint8_t *dst = (uint8_t *)buf;

	if (!f || !f->accessed || off < 0)
		return -1;
	if ((unsigned long)off >= f->valid)
		return 0;                     /* at/after EOF -> 0 bytes */

	avail = f->valid - (unsigned long)off;
	if (n > avail)
		n = avail;                    /* clamp; readvb never over-reads */
	if (n == 0)
		return 0;

	/*
	 * Chunk the transfer into <= 1 MiB IO$_READVBLKs. A single PT_LOAD of a
	 * producer/image (DECC$SHR is already ~1 MiB and grows with the CRTL)
	 * exceeds the executive's per-QIO bound in one call; issuing it whole
	 * drew SS$_BADPARAM -> IMGACT open failed with %IMGACT-F-IMGNOTFND
	 * (vms-1162). vbn/offset are re-derived from the running byte cursor each
	 * pass, so block-boundary crossing is handled exactly as the single call
	 * did (the executive readvb itself spans blocks within a chunk).
	 */
	remaining = n;
	cur_off   = (unsigned long)off;
	while (remaining > 0) {
		struct vms_acp_rw_args r;
		uint32_t chunk = remaining > IMGACT_ACP_RW_MAX_XFER
				     ? IMGACT_ACP_RW_MAX_XFER
				     : (uint32_t)remaining;

		acp_memset(&r, 0, sizeof(r));
		r.chan   = f->chan;
		r.vbn    = (uint32_t)(cur_off / 512u) + 1u;
		r.offset = (uint32_t)(cur_off % 512u);
		r.length = chunk;
		r.buffer = (uint64_t)(uintptr_t)dst;

		if (imgact_acp_dev_ioctl(f->dev_fd, VMS_IOCTL_ACP_READVBLK, &r) < 0)
			return -1;
		if (r.status == SS$_ENDOFFILE)
			break;                /* clamped read shouldn't reach here, but stop */
		if (!$VMS_STATUS_SUCCESS(r.status))
			return -1;

		xferred   += r.xferred;
		dst       += r.xferred;
		cur_off   += r.xferred;
		remaining -= r.xferred;
		if (r.xferred < chunk)
			break;                /* short read -> EOF; return accumulated */
	}

	return (long)xferred;
}

void imgact_acp_close(struct imgact_acp_file *f)
{
	if (!f || f->dev_fd < 0)
		return;
	if (f->accessed)
		acp_deaccess(f->dev_fd, f->chan);
	if (f->chan)
		acp_dassgn(f->dev_fd, f->chan);
	imgact_acp_dev_close(f->dev_fd);
	f->dev_fd = -1;
	f->accessed = 0;
	f->chan = 0;
}
