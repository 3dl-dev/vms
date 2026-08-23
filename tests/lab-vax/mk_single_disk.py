#!/usr/bin/env python3
#
# mk_single_disk.py - vms-7b15: finish a SINGLE-disk OVMX/NetBSD-vax image on the
# HOST. Given an image whose root FFS has ALREADY been shrunk in-guest
# (drive_boot_vax.py assemble-single -> resize_ffs on /dev/rra1a), this:
#
#   1. rewrites the NetBSD disklabel (at LABELSECTOR 0 / LABELOFFSET 64 on vax):
#      * SHRINKS partition 'a' (the root FFS) to A_SECTORS -- matching the
#        resize_ffs done in-guest, so 'a' no longer claims the space 'e' needs;
#      * ADDS partition 'e' = the raw OVMX ODS-2 volume, at offset A_SECTORS,
#        size = the ODS-2 image's sector count (FS_OTHER, so no fsck ever touches
#        it -- the executive opens /dev/ra0e as a RAW block device);
#      * leaves 'b' (swap, at the tail) and 'c' (whole disk) untouched;
#      * fixes d_npartitions and the disklabel checksum (dkcksum).
#   2. writes the ODS-2 volume bytes into partition 'e' (offset A_SECTORS*512).
#
# Pure on-disk struct + dd, no NetBSD tools needed and fully deterministic, which
# is why it runs host-side rather than in the slow emulator. NetBSD reads this
# label at boot (readdisklabel), so /dev/ra0e names exactly the ODS-2 region.
#
# This is OVMX's own code against the PUBLIC, documented NetBSD `struct
# disklabel' on-disk layout (sys/sys/disklabel.h) -- no third-party source is
# copied (CLAUDE.md Rule 8). It touches no VMS format at all; the ODS-2 volume it
# injects was mastered separately by tools/vmsfs_master.c.

import struct
import sys

DISKMAGIC = 0x82564557          # sys/disklabel.h DISKMAGIC
LABELSECTOR = 0                 # vax: label lives in sector 0 ...
LABELOFFSET = 64                # ... at byte offset 64
# struct disklabel field offsets, RELATIVE to the label start. These are fixed by
# the on-disk layout: d_magic(0), ..., d_magic2 sits 132 bytes in, and the
# partition array begins 148 bytes in (16 bytes/partition).
OFF_MAGIC2 = 132
OFF_CKSUM = 136                 # d_checksum (u16)
OFF_NPART = 138                 # d_npartitions (u16)
OFF_PARTS = 148                 # d_partitions[0]
PART_SZ = 16                    # sizeof(struct partition)
SECSIZE = 512
OFF_SECPERCYL = 56             # d_secpercyl (u32), rel to label start
OFF_NCYL = 52                  # d_ncylinders (u32)
OFF_SECPERUNIT = 60            # d_secperunit (u32)

# fstype codes (sys/disklabel.h)
FS_UNUSED = 0
FS_OTHER = 10                   # "in use, but unknown/unsupported" -> never fsck'd


def _find_label_off(buf):
    """Locate the disklabel: DISKMAGIC at LABELOFFSET of sector LABELSECTOR, and
    a matching second magic 132 bytes further in. Fall back to a scan of sector 0
    if the canonical offset does not match (belt and braces)."""
    cand = LABELSECTOR * SECSIZE + LABELOFFSET
    m = struct.pack("<I", DISKMAGIC)
    if buf[cand:cand + 4] == m and buf[cand + OFF_MAGIC2:cand + OFF_MAGIC2 + 4] == m:
        return cand
    # scan the first sector
    for off in range(0, SECSIZE):
        if buf[off:off + 4] == m and buf[off + OFF_MAGIC2:off + OFF_MAGIC2 + 4] == m:
            return off
    raise SystemExit("mk_single_disk: no NetBSD disklabel (DISKMAGIC) found")


def _dkcksum(buf, lab, npart):
    """dkcksum: XOR of every u16 word from the label start through the last
    partition, with d_checksum already zeroed (sys/lib/libkern/... dkcksum)."""
    end = lab + OFF_PARTS + npart * PART_SZ
    s = 0
    for off in range(lab, end, 2):
        s ^= struct.unpack_from("<H", buf, off)[0]
    return s & 0xffff


def _part(buf, lab, idx):
    base = lab + OFF_PARTS + idx * PART_SZ
    size, offset, fsize = struct.unpack_from("<III", buf, base)
    fstype = buf[base + 12]
    return size, offset, fstype


def _set_part(buf, lab, idx, size, offset, fsize, fstype, frag, cpg):
    base = lab + OFF_PARTS + idx * PART_SZ
    struct.pack_into("<III", buf, base, size, offset, fsize)
    buf[base + 12] = fstype & 0xff
    buf[base + 13] = frag & 0xff
    struct.pack_into("<H", buf, base + 14, cpg & 0xffff)


def main(argv):
    if len(argv) < 5:
        sys.exit("usage: mk_single_disk.py SINGLE_IMG ODS2_IMG A_SECTORS "
                 "TOTAL_SECTORS [E_INDEX=4]\n"
                 "  TOTAL_SECTORS: slim disk size (whole-disk 'c' + secperunit + "
                 "file truncation); 0 = keep the original disk size")
    single_img = argv[1]
    ods2_img = argv[2]
    a_sectors = int(argv[3])
    total_sectors = int(argv[4])
    e_idx = int(argv[5]) if len(argv) > 5 else 4     # partition 'e'

    with open(ods2_img, "rb") as f:
        ods2 = f.read()
    e_sectors = (len(ods2) + SECSIZE - 1) // SECSIZE
    if len(ods2) % SECSIZE:
        # pad the last partial sector so the whole ODS-2 volume lands intact
        ods2 = ods2 + b"\0" * (e_sectors * SECSIZE - len(ods2))

    with open(single_img, "r+b") as f:
        head = bytearray(f.read(SECSIZE * 2))
        lab = _find_label_off(head)
        npart = struct.unpack_from("<H", head, lab + OFF_NPART)[0]
        secperunit = struct.unpack_from("<I", head, lab + 40 + 20)[0]  # d_secperunit
        print("mk_single_disk: label at byte %d, npartitions=%d, secperunit=%d "
              "(%.1f MiB)" % (lab, npart, secperunit,
                              secperunit * SECSIZE / 1048576.0))

        a_size, a_off, a_fs = _part(head, lab, 0)
        c_size, c_off, c_fs = _part(head, lab, 2)
        secpercyl = struct.unpack_from("<I", head, lab + OFF_SECPERCYL)[0]
        # keep 'a's fsize/frag/cpg; only its p_size changes
        a_base = lab + OFF_PARTS + 0 * PART_SZ
        a_fsize = struct.unpack_from("<I", head, a_base + 8)[0]
        a_frag = head[a_base + 13]
        a_cpg = struct.unpack_from("<H", head, a_base + 14)[0]

        # The effective disk size: the slim TOTAL_SECTORS if given, else the
        # original secperunit.
        disk_sectors = total_sectors if total_sectors > 0 else secperunit

        e_off = a_sectors
        e_end = e_off + e_sectors
        print("mk_single_disk: a: %d->%d sectors (%.1f MiB); e: off %d size %d "
              "(%.1f MiB) end %d; disk %d sectors (%.1f MiB)" %
              (a_size, a_sectors, a_sectors * SECSIZE / 1048576.0,
               e_off, e_sectors, e_sectors * SECSIZE / 1048576.0, e_end,
               disk_sectors, disk_sectors * SECSIZE / 1048576.0))

        # sanity: 'e' must fit inside the (slim) disk.
        if e_end > disk_sectors:
            sys.exit("mk_single_disk: partition 'e' (end %d) exceeds the disk "
                     "(%d sectors)" % (e_end, disk_sectors))
        if a_sectors >= a_size:
            sys.exit("mk_single_disk: refusing to GROW 'a' (%d >= %d) -- the "
                     "in-guest resize_ffs must have shrunk it first" %
                     (a_sectors, a_size))

        # shrink 'a', add 'e', ensure 'd' (index 3) is a benign unused slot if we
        # are extending npartitions past it.
        _set_part(head, lab, 0, a_sectors, a_off, a_fsize, a_fs, a_frag, a_cpg)
        if e_idx >= 4 and npart <= 3:
            _set_part(head, lab, 3, 0, 0, 0, FS_UNUSED, 0, 0)   # 'd' unused
        _set_part(head, lab, e_idx, e_sectors, e_off, 0, FS_OTHER, 0, 0)

        if total_sectors > 0:
            # SLIM: the whole-disk 'c' now spans the slim disk, the swap 'b'
            # (which sat at the 2 GiB tail) falls OFF the slim disk so it is
            # dropped, and the geometry (secperunit / ncylinders) is made
            # consistent with the smaller media so readdisklabel is happy.
            _set_part(head, lab, 2, total_sectors, 0, 0, FS_UNUSED, 0, 0)  # 'c'
            b_size, b_off, b_fs = _part(head, lab, 1)
            if b_off >= total_sectors or b_off + b_size > total_sectors:
                _set_part(head, lab, 1, 0, 0, 0, FS_UNUSED, 0, 0)  # drop swap
            struct.pack_into("<I", head, lab + OFF_SECPERUNIT, total_sectors)
            if secpercyl > 0:
                struct.pack_into("<I", head, lab + OFF_NCYL,
                                 total_sectors // secpercyl)

        new_npart = max(npart, e_idx + 1)
        struct.pack_into("<H", head, lab + OFF_NPART, new_npart)
        struct.pack_into("<H", head, lab + OFF_CKSUM, 0)         # zero before sum
        cks = _dkcksum(head, lab, new_npart)
        struct.pack_into("<H", head, lab + OFF_CKSUM, cks)
        print("mk_single_disk: npartitions %d->%d, new dkcksum=0x%04x"
              % (npart, new_npart, cks))

        # write the label back
        f.seek(0)
        f.write(head)
        # write the ODS-2 volume into partition 'e'
        f.seek(e_off * SECSIZE)
        f.write(ods2)
        f.flush()
        if total_sectors > 0:
            # TRUNCATE the image to the slim disk size -- the whole point of the
            # slim artifact. Everything used (FFS 'a' + ODS-2 'e') lives below
            # total_sectors, so nothing is lost.
            f.truncate(total_sectors * SECSIZE)
    print("mk_single_disk: wrote %d ODS-2 bytes into partition 'e' at offset %d; "
          "single disk ready" % (len(ods2), e_off * SECSIZE))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
