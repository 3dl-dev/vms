/*
 * scs_mscp_srv.h - MSCP DISK SERVER responder (vms-291, MSCP epic Phase D).
 *
 * scs_mscp.c is the disk CLIENT: it builds the commands OVMX sends to a real
 * VAX's MSCP server. THIS module is the other half -- it answers commands a
 * real VAX's disk class driver (VMS$DISK_CL_DRVR) sends to OVMX, so that a real
 * VAX can MOUNT a disk OVMX serves.
 *
 * =========================== WHY THIS EXISTS ==============================
 *
 * OVMX already ACCEPTS the member-opened MSCP$DISK connect (op=1 echo, op=4
 * accept, behind OVMX_MSCP_SERVER -- see ovmx_mscp_server_enabled() in scsd.c)
 * and then cannot serve a single command on it. An accepted connection that
 * black-holes every command is precisely the dishonest-success shape INV-6
 * exists to kill, one layer up. This module is what makes that acceptance
 * truthful.
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md Rule 8). Every offset, opcode, status code
 * and end-message layout below is transcribed from the *MSCP Basic Disk
 * Functions Manual* AA-L619A-TK v1.2 (Apr 1982) -- the customer-orderable UDA50
 * Programmer's Doc Kit QP-905-GZ, standard copyright page, no confidential or
 * restricted-distribution marking, therefore admissible public documentation.
 * Sections and tables are cited inline. The conformance oracle is our own lab
 * wire (see GOLDEN END MESSAGES below).
 * NOTHING here was disassembled, decompiled, or read from VSI/HPE source.
 * The bitsavers `dec/dsa/mscp` files are stamped DEC CONFIDENTIAL AND
 * PROPRIETARY / RESTRICTED DISTRIBUTION and were NOT read.
 *
 * ====================== GOLDEN END MESSAGES (vms-291) ======================
 *
 * Phase B could prove its command builders wire-neutral because the corpus held
 * real captured joiner COMMANDS. Phase D can do the same for END MESSAGES,
 * because the corpus holds a real VAX's MSCP server ANSWERS.
 *
 * ATTRIBUTION, because the easy version of this sentence is wrong: the GUS end
 * class (110-content, endcode 0x83) was ALREADY identified and fully decoded by
 * vms-4eb -- docs/cluster-protocol-spec.md sec 4(h)(1e), every Table A-7 field
 * re-derived from the wire -- and the 0x84 SCC endcode was already recorded in
 * sec 4(n). What this item adds is the SCC END class's own census and the
 * PER-FIELD CONSTANCY of its parameter area (below), which is what makes a
 * byte-exact builder possible rather than merely a plausible one.
 *
 * Censused over all 489 lab pcaps, filtered to SCS MTYPE 10 (the p. 4-13
 * application message) and read at body[8]:
 *
 *   SCA content 86   MTYPE 10   body[8] = 0x84   954 frames   SCC END
 *   SCA content 110  MTYPE 10   body[8] = 0x83  18855 frames  GUS END
 *
 * 0x84 == OP.SCC|OP.END and 0x83 == OP.GUS|OP.END (Table A-1). The counts pair
 * with their commands in the same corpus -- 954 SCC commands against 954 SCC
 * ENDs is exact -- which is what identifies the classes rather than merely
 * being consistent with them. tests/vmsscs/test_scs_mscp_srv.c holds byte-exact
 * bodies from af2-firsttimer-established-20260728.pcap and requires this
 * module's builders to reproduce them.
 *
 * WHAT THE *JOINER* CORPUS DOES NOT CONTAIN: ONLINE (0x89) and READ (0xa1) end
 * messages, and the SCA block data transfer that moves disk blocks. The
 * captured joiner only ever performs SCC + GET UNIT STATUS discovery; it never
 * mounts. THIS ITEM WENT AND CAPTURED THEM -- a real VAX serving a disk to
 * another real VAX on lab-2 -- and the result is in the WIRE FIDELITY section
 * further down, which is the authority on what is now grounded and what is
 * still book-only. Read that before trusting any statement here about ONLINE,
 * READ or block transfer.
 *
 * ======================= DESIGN DECISIONS (vms-291) ========================
 *
 * (1) BACKING STORE = A RAW BLOCK IMAGE FILE, AND OVMX'S OWN vmsfs IS NOT USED.
 *     MSCP is a BLOCK protocol: it addresses logical blocks and knows nothing
 *     about file systems (AA-L619A-TK sec 5.3 -- byte count, buffer descriptor,
 *     logical block number, and no notion of a file anywhere). A served unit is
 *     therefore just a file of 512-byte blocks and this module is deliberately
 *     filesystem-agnostic.
 *
 *     This is NOT an arbitrary choice. src/kernel/vmsfs/vmsfs_ondisk.h states in
 *     its own header comment that its layout is "Simplified ODS-2-inspired ...
 *     our own on-disk structures -- not byte-compatible with real ODS-2". A real
 *     VAX mounting a served unit runs the Fig 4-38 (p. 4-106) mount
 *     verification -- ONLINE, GET UNIT STATUS, read the homeblock, read the SCB
 *     -- and parses genuine ODS-2 structures. Serving an OVMX vmsfs volume could
 *     therefore never mount, and "make vmsfs byte-compatible with ODS-2" is a
 *     different, much larger project than this one.
 *     So the volume CONTENT must be a genuine ODS-2 volume produced by real VMS
 *     (a lab disk image, or an image a lab VAX has INITIALIZEd). OVMX serves its
 *     blocks verbatim. That keeps Phase D's scope on the PROTOCOL, where it
 *     belongs, and leaves OVMX's on-disk format story completely untouched.
 *
 * (2) READ-ONLY IN v1, AND SAID SO ON THE WIRE RATHER THAN SILENTLY.
 *     A served unit is opened read-only and advertises UF.WPS -- Software Write
 *     Protect, Table A-5 bit 12 -- in the unit flags of its ONLINE and GET UNIT
 *     STATUS end messages, so the class driver is TOLD the volume is
 *     write-protected before it ever tries to write. A WRITE that arrives
 *     anyway is answered with the exact spec status for the situation:
 *     Write Protected, sub-code "Unit is Software Write Protected"
 *     (Table B-2: 0x1006). It is never silently dropped and never falsely
 *     acknowledged.
 *
 *     WHY read-only: the write direction needs the OPPOSITE half of block data
 *     transfer (REQDAT -- the server PULLS data out of a named host buffer),
 *     which is a distinct and ungrounded wire shape, and a mis-implemented WRITE
 *     corrupts a real volume rather than merely failing. Read-only still
 *     delivers the whole headline capability -- a real VAX mounting and reading
 *     files from a disk OVMX serves. Read-write is explicit follow-up scope.
 *
 * (3) SERVER DATA STRUCTURES, SCOPED DOWN ON PURPOSE.
 *     VAXcluster Principles pp. 4-114..4-117 gives the shape of a real VMS
 *     serving implementation: DSRV/TSRV, UQB per served unit, HQB per remote
 *     class driver, HULB per host/unit for load balancing, HRB per outstanding
 *     remote request. That is a TEMPLATE, not a mandate. This module keeps the
 *     two that carry state OVMX actually has -- struct scs_mscp_srv_unit is the
 *     UQB analogue (one served unit) and struct scs_mscp_srv_host is the HQB
 *     analogue (one remote class driver's controller-online state, controller
 *     flags and host timeout). It deliberately implements NO HULB: OVMX serves
 *     from one node with no second server to balance against, so load-balancing
 *     machinery would be code no test could exercise. The HRB (per outstanding
 *     request) collapses because command handling here is synchronous -- the
 *     backing store is a local file, so a command is answered in the call that
 *     receives it and nothing is ever outstanding.
 *
 * (4) A READ THAT CANNOT MOVE ITS DATA FAILS LOUDLY.
 *     AA-L619A-TK sec 5.3: a transfer command carries a 12-byte buffer
 *     descriptor naming a HOST buffer, and the data crosses by the
 *     communications mechanism's own block transfer service -- for SCA, the
 *     named-buffer SNDDAT/REQDAT machinery of VAXcluster Principles
 *     pp. 2-32..2-41. OVMX HAS NO WIRE-GROUNDED IMPLEMENTATION OF THAT YET
 *     (rd vms-941; no capture in our corpus shows one, because the captured
 *     joiner never mounts anything).
 *     So scs_mscp_srv_read() reads the blocks out of the backing store -- that
 *     part is real and tested -- and hands them to a caller-installed transfer
 *     hook. WITH NO HOOK INSTALLED IT RETURNS A CONTROLLER ERROR END MESSAGE,
 *     not a Success end message with no data behind it. Reporting a successful
 *     READ for bytes that never crossed is the exact INV-6 failure this project
 *     forbids, and the refusal is asserted by a unit test so it cannot regress
 *     into a fake success later.
 *
 * ============ WIRE FIDELITY AFTER THE vms-291 SERVING CAPTURE =============
 *
 * A real VAX serving a disk to another real VAX was captured on lab-2
 * (vaxlab-9) for this item -- the first such capture the project holds.
 * VAX2 ran MOUNT against a unit VAX1 served and read a marker file back, and
 * the whole dialogue is on the wire. Artifacts (host-only, NOT in git):
 *   /data/training/vax/k8s-labs/vaxlab-9/logs/vms291-mount-A.pcap   (the mount)
 *   /data/training/vax/k8s-labs/vaxlab-9/logs/vms291-control-B.pcap (the control)
 *   /data/training/vax/k8s-labs/vaxlab-9/logs/vms291-boot-C.pcap    (cold join)
 * The control is what makes the mount capture attributable: with the volume
 * dismounted and MOUNT aimed at a nonexistent unit, it carries ZERO READ, ZERO
 * WRITE, ZERO block-transfer frames and zero marker bytes.
 *
 * WHAT THAT SETTLED, and what this module now does because of it:
 *
 *  - END MESSAGE LENGTHS. Measured: SCC 28, ONLINE 44, READ 32, WRITE 36,
 *    GUS 52. Three of those this module already had right from Table A-7 alone.
 *    TWO IT HAD WRONG and both are fixed here: GUS was 48 (Table A-7's last
 *    field) where every real GUS end is 52, and WRITE was assumed equal to
 *    READ where a real server declares four bytes more. Mutation arms pin both.
 *  - THE GUS TAIL. body[48:50] is 0x006e on every GUS end ever measured,
 *    including an RA81 on a different controller -- so the capture did NOT
 *    break the length-echo/plain-constant tie. It DID show the field is written
 *    deliberately: in invalid-unit replies the surrounding tail carries visible
 *    stale garbage while [48:50] still reads 0x006e. So OVMX copies [48:50] and
 *    leaves [50:52] zero. See SCS_MSCP_E_GUS_TAIL_OBSERVED.
 *  - P.UNFL BIT 15 IS HOST-ORIGINATED, which resolves it as a design question
 *    without decoding it. The class driver's ONLINE COMMAND carries 0x8000 and
 *    the server ECHOES it; the bit is not something a controller invents. This
 *    module now echoes the host's unit-flags word (OR-ed with the unit's own
 *    non-host-settable flags, so a host cannot clear write protection by
 *    asking). Its MEANING is still undecoded and is still not named.
 *
 * WHAT REMAINS UNGROUNDED -- do not build on these:
 *  - The block-transfer header's field at +4 (constant per connection, 9 on one
 *    connection and 13 on another) and at +6 (constant per transfer). Neither
 *    is decoded. In particular +4 is NOT a message type, despite looking like
 *    one.
 *  - The SCC controller-flags bits 15/13/11/2 and P.UNFL bit 15's meaning.
 *  - Whether MSCP credit/flow control interacts with block transfers.
 *
 * AND THE BIG ONE: BLOCK DATA TRANSFER IS DECODED BUT NOT IMPLEMENTED HERE.
 * The capture grounds the wire format (see docs/design-mscp-direction.md for
 * the field table), so vms-941 is now buildable -- but this module still ships
 * with no transfer hook, and a READ therefore still answers Controller Error
 * rather than a Success it cannot back up. Implementing it is part 2.
 * MSCP$DISK is still NOT registered via LISTEN (vms-61b2) for the same reason:
 * OVMX cannot yet honour a mount, so advertising that it can would be the
 * facade vms-61b2 refused to build.
 */
#ifndef SCS_MSCP_SRV_H
#define SCS_MSCP_SRV_H

#include <stddef.h>
#include <stdint.h>

#include "scs_mscp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- End-message body offsets, AA-L619A-TK Table A-7. Body-relative, i.e.
 * from SCA content[58]. The generic header (P.CRF/P.UNIT/P.OPCD/P.FLGS/P.STS)
 * is shared with commands and already named in scs_mscp.h; only the per-command
 * END parameter areas are new here. ------------------------------------- */

/* Generic end message (Table A-7). */
#define SCS_MSCP_E_BCNT 12 /* byte count, 4 -- transfer commands */
#define SCS_MSCP_E_FBBK 28 /* first bad block, 4 */

/* SET CONTROLLER CHARACTERISTICS end message (Table A-7). */
#define SCS_MSCP_E_VRSN 12 /* MSCP version, 2 */
#define SCS_MSCP_E_CNTF 14 /* controller flags, 2 */
#define SCS_MSCP_E_CTMO 16 /* controller timeout, 2 */
#define SCS_MSCP_E_RSVD18 18 /* Table A-7: RESERVED. A real VMS server sends
                              * 0x0547 here on 954/954 captured frames -- see
                              * ctlr_version_word below. */
#define SCS_MSCP_E_CNTI 20 /* controller identifier, 8 */
#define SCS_MSCP_SCC_END_LEN 28 /* == the captured 86-content class body */

/* GET UNIT STATUS end message (Table A-7). */
#define SCS_MSCP_E_MLUN 12 /* multi-unit code, 2 */
#define SCS_MSCP_E_UNFL 14 /* unit flags, 2 */
#define SCS_MSCP_E_UNTI 20 /* unit identifier, 8 */
#define SCS_MSCP_E_MEDI 28 /* media type identifier, 4 */
#define SCS_MSCP_E_SHUN 32 /* shadow unit, 2 */
#define SCS_MSCP_E_TRCK 36 /* track size, 2 */
#define SCS_MSCP_E_GRP  38 /* group size, 2 */
#define SCS_MSCP_E_CYL  40 /* cylinder size, 2 */
#define SCS_MSCP_E_RCTS 44 /* RCT table size, 2 */
#define SCS_MSCP_E_RBNS 46 /* RBNs per track, 1 */
#define SCS_MSCP_E_RCTC 47 /* RCT copies, 1 */
/* Table A-7's last GUS field (P.RCTC) ends at 48, but a REAL VMS server emits
 * 52 -- 110 SCA content, measured on every GUS end message in the corpus AND in
 * the vms-291 serving capture, including for an RA81 whose geometry and media
 * id differ from the RA92s. So 48 would be a length no VAX has ever emitted.
 * See SCS_MSCP_E_GUS_TAIL below for what is in the extra four bytes. */
#define SCS_MSCP_GUS_END_LEN 52
#define SCS_MSCP_E_GUS_TAIL  48 /* the 4 bytes past Table A-7, [48:52] */

/* body[48:50] == 0x006e on every GUS end message ever measured.
 *
 * WHY THIS IS COPIED AND NOT COMPOSED. 0x6e is 110, which is this class's own
 * SCA content length, and every frame of the class is that length -- so a
 * length-echo reading and a plain-constant reading are INDISTINGUISHABLE, and
 * the spec (sec 5 register) forbids naming it or emitting a guess. The vms-291
 * serving capture did NOT break the tie: an RA81 on a different controller
 * still produced 110.
 *
 * What that capture DID establish is that the field is written DELIBERATELY.
 * In invalid-unit replies (status 0x0003) the surrounding tail carries visible
 * stale buffer garbage -- one reply's body[36:48] decodes as the ASCII "V5.0"
 * fragment, and body[50:52] takes junk values like 0x4843 / 0x4231 -- while
 * body[48:50] still reads 0x006e. That kills the "it is all uninitialised
 * padding" reading, which is progress, and still cannot separate the other two.
 *
 * So OVMX emits the OBSERVED BYTES at [48:50] and ZERO at [50:52]: the one
 * field a real server always writes is reproduced, and the one it demonstrably
 * leaves as garbage is not filled with an invention. */
#define SCS_MSCP_E_GUS_TAIL_OBSERVED 0x006eu

/* ONLINE / SET UNIT CHARACTERISTICS end message (Table A-7). Shares P.MLUN,
 * P.UNFL, P.UNTI and P.MEDI with the GUS end message above, then diverges. */
#define SCS_MSCP_E_UNSZ 36 /* unit size in logical blocks, 4 */
#define SCS_MSCP_E_VSER 40 /* volume serial number, 4 */
#define SCS_MSCP_ONLINE_END_LEN 44 /* == the captured 102-content class body */

/* TRANSFER end messages. Table A-7's generic end message ends at 32 (P.FBBK,
 * 28..32) and a real server's READ end declares exactly that -- but its WRITE
 * end declares 36, on every one measured in the vms-291 serving capture. The
 * extra four bytes are NOT explained by the manual and OVMX zero-fills them
 * rather than inventing content; only the LENGTH is reproduced.
 *
 * (A READ end message's SCA content varies -- 118/194/448/630/1142 observed --
 * because a real server PIGGYBACKS the transfer's trailing data into the same
 * Ethernet frame, past the declared MSCP length. The MSCP message itself is 32
 * in every case, and 32 is what this builder produces.) */
#define SCS_MSCP_READ_END_LEN  32
#define SCS_MSCP_WRITE_END_LEN 36

/* The longest end message this module builds. */
#define SCS_MSCP_SRV_END_MAX 52

/* Command modifiers this responder honours beyond scs_mscp.h's MD.NXU
 * (Table A-2). */
#define SCS_MSCP_MOD_IGNORE_MEDIA_FMT 0x0002u /* MD.IMF (ONLINE) */
#define SCS_MSCP_MOD_SET_WRITE_PROT   0x0004u /* MD.SWP (ONLINE) */

/* Unit flags, Table A-5. Only the ones this server sets or tests. */
#define SCS_MSCP_UF_COMPARE_READS  0x0001u /* UF.CMR */
#define SCS_MSCP_UF_COMPARE_WRITES 0x0002u /* UF.CMW */
#define SCS_MSCP_UF_576_SECTORS    0x0004u /* UF.576 */
#define SCS_MSCP_UF_REMOVABLE      0x0080u /* UF.RMV */
#define SCS_MSCP_UF_WRITE_PROT_SW  0x1000u /* UF.WPS -- software write protect */
#define SCS_MSCP_UF_WRITE_PROT_HW  0x2000u /* UF.WPH -- hardware write protect */

/* Status words this responder emits, composed as Appendix B note 1 requires:
 * (subcode * ST.SUB) + code, i.e. (subcode << 5) | major. Spelled out rather
 * than left as bare numbers because a status word is NOT a flat code -- see the
 * warning on SCS_MSCP_ST_MASK in scs_mscp.h. */
#define SCS_MSCP_STATUS(major, subcode) \
    ((uint16_t)((((uint16_t)(subcode)) << SCS_MSCP_ST_SUB_SHIFT) \
                | ((uint16_t)(major) & SCS_MSCP_ST_MASK)))

/* Table B-2 "Success" sub-codes. */
#define SCS_MSCP_SUB_NORMAL        0u
#define SCS_MSCP_SUB_ALREADY_ONLINE 8u
/* Table B-2 "Unit-Offline" sub-codes. */
#define SCS_MSCP_SUB_OFL_UNKNOWN   0u /* unit unknown or online elsewhere */
#define SCS_MSCP_SUB_OFL_NO_VOLUME 1u /* no volume mounted / drive disabled */
/* Table B-2 "Write Protected" sub-codes. */
#define SCS_MSCP_SUB_WP_SOFTWARE   128u /* -> 0x1006 */
#define SCS_MSCP_SUB_WP_HARDWARE   256u /* -> 0x2006 */
/* Table B-3 "Controller Error" sub-code 3, "Inconsistent internal data
 * structure" -- the closest published code for "this controller cannot complete
 * the transfer", which is what an absent block-transfer hook means. */
#define SCS_MSCP_SUB_CNT_INCONSISTENT 3u

/* AA-L619A-TK sec 5.3: a transfer's byte count must be a whole number of
 * blocks and must not run off the end of the unit. */
#define SCS_MSCP_BLOCK_SIZE 512u

/* Sizing. A single-unit server is the shipped configuration; the array exists
 * so the GET UNIT STATUS MD.NXU walk has something to walk. */
#define SCS_MSCP_SRV_MAX_UNITS 8

/*
 * scs_mscp_srv_unit - ONE SERVED UNIT. The UQB analogue of pp. 4-114..4-117,
 * carrying only state OVMX actually has.
 */
struct scs_mscp_srv_unit {
    uint16_t unit;        /* P.UNIT -- the unit select number we answer for */
    int      in_use;      /* slot occupied */
    int      online;      /* an ONLINE command has brought it Unit-Online */
    int      read_only;   /* v1: always 1; see design decision (2) */

    /* The backing store. A raw image of `unit_size` 512-byte logical blocks.
     * `fd` is a read-only descriptor owned by the caller of
     * scs_mscp_srv_attach_fd(); this module never closes it. */
    int      fd;
    uint32_t unit_size;   /* P.UNSZ -- logical blocks on the volume */

    /* Characteristics reported by GET UNIT STATUS / ONLINE end messages. */
    uint64_t unit_id;     /* P.UNTI -- sec 6.12: non-zero iff characteristics valid */
    uint32_t media_id;    /* P.MEDI */
    uint16_t unit_flags;  /* P.UNFL, Table A-5 */
    uint32_t volume_ser;  /* P.VSER */
    uint16_t multi_unit;  /* P.MLUN -- low byte access path, high byte spindle */
    uint16_t track_size;  /* P.TRCK */
    uint16_t group_size;  /* P.GRP */
    uint16_t cyl_size;    /* P.CYL */
    uint16_t rct_size;    /* P.RCTS */
    uint8_t  rbns;        /* P.RBNS */
    uint8_t  rct_copies;  /* P.RCTC */
};

/*
 * scs_mscp_srv_xfer_fn - THE BLOCK DATA TRANSFER HOOK. See design decision (4).
 *
 * Called with the bytes a READ produced and the 12-byte buffer descriptor
 * (sec 5.3) naming the host buffer they must be delivered into. Returns the
 * number of bytes actually transferred, or -1 if the transfer failed.
 *
 * THERE IS NO DEFAULT IMPLEMENTATION and that is deliberate: SCA block data
 * transfer is not wire-grounded yet (rd vms-941), and a stub that returned
 * `len` without moving anything would make every READ report a success for data
 * that never crossed the wire.
 */
typedef long (*scs_mscp_srv_xfer_fn)(void *ctx, const uint8_t buffer_desc[12],
                                     uint32_t lbn, const uint8_t *data,
                                     size_t len);

/*
 * scs_mscp_srv_host - ONE REMOTE CLASS DRIVER. The HQB analogue.
 * Per-connection MSCP state: sec 6.16 makes controller flags and host timeout
 * per-class-driver, and sec 3.4 makes "Controller-Online" a per-class-driver
 * condition, so this cannot live on the unit.
 */
struct scs_mscp_srv_host {
    int      in_use;
    uint32_t conid;        /* the SCS Con.ID pair identifying this class driver */
    int      ctlr_online;  /* a SET CONTROLLER CHARACTERISTICS has completed */
    uint16_t ctlr_flags;   /* P.CNTF as the host set it (Table A-4) */
    uint16_t host_timeout; /* P.HTMO in seconds; 0 == disabled (sec 6.16) */
};

#define SCS_MSCP_SRV_MAX_HOSTS 4

/*
 * scs_mscp_srv - the server. The DSRV analogue.
 */
struct scs_mscp_srv {
    struct scs_mscp_srv_unit  units[SCS_MSCP_SRV_MAX_UNITS];
    struct scs_mscp_srv_host  hosts[SCS_MSCP_SRV_MAX_HOSTS];

    /* P.CNTI, reported in the SCC end message. OBSERVED SHAPE, recorded in
     * scs_mscp_srv.c: on the lab wire a VMS server's controller identifier
     * carries its own node logical-address suffix in the low word. */
    uint64_t ctlr_id;
    /* P.CTMO, the controller timeout the server reports back (sec 6.16: a
     * 16-bit field but effectively <= 255 seconds). */
    uint16_t ctlr_timeout;

    /* ---- THE SCC END MESSAGE'S TWO UNDECODED OBSERVED VALUES ------------
     * Measured over every SET CONTROLLER CHARACTERISTICS end message in the
     * lab corpus -- 954 of 954 frames, no variation:
     *
     *   P.CNTF  [14:16] = 0xa004    ctlr_flags_reported
     *   [18:20]         = 0x0547    ctlr_version_word
     *
     * NEITHER IS EXPLAINED BY THE 1982 MANUAL, and saying so is the point of
     * these two fields existing separately from the values a host sets:
     *  - 0xa004 sets bits 15, 13 and 2. Table A-4 defines controller flags at
     *    bits 7, 6, 5, 4 and 0 ONLY, so this is not a Table A-4 flag word, and
     *    it is NOT an echo of the flags the class driver sent (our own client
     *    sends CF.ATN|CF.MSC|CF.THS == 0xd0 and gets 0xa004 back).
     *  - Table A-7 calls [18:20] RESERVED and sec 5.2 requires a server to
     *    supply 0 there. A real VMS 7.3 server does not; it sends 0x0547.
     * The plain reading is that both carry controller identity/capability
     * information added in an MSCP revision later than AA-L619A-TK v1.2, but
     * that is a HYPOTHESIS and nothing in our corpus decodes it. Recorded as
     * observations, not named as fields they have not been shown to be.
     *
     * DEFAULTS ARE THE SPEC'S, NOT THE OBSERVATION'S: scs_mscp_srv_init()
     * leaves both 0, which is what sec 5.2 requires of a reserved field.
     * scs_mscp_srv_set_ctlr_profile() loads the observed values for callers
     * that want to look like the VMS server on our wire -- and it is what the
     * byte-exact golden test uses. */
    uint16_t ctlr_flags_reported;
    uint16_t ctlr_version_word;

    scs_mscp_srv_xfer_fn xfer;
    void                *xfer_ctx;

    /* Counters. These are the kill-switch instruments: a behaviour that claims
     * to have happened must have moved one of these. */
    unsigned long cmds_received;
    unsigned long ends_sent;
    unsigned long blocks_read;
    unsigned long writes_refused;
    unsigned long xfer_refusals; /* READs failed for want of a transfer hook */
};

/*
 * scs_mscp_srv_init - zero the server and load the two controller-level values
 * it reports. `ctlr_id` is P.CNTI; `ctlr_timeout` is P.CTMO in seconds.
 * Returns 0, or -1 if srv is NULL.
 */
int scs_mscp_srv_init(struct scs_mscp_srv *srv, uint64_t ctlr_id,
                      uint16_t ctlr_timeout);

/*
 * scs_mscp_srv_set_ctlr_profile - set the two SCC-end-message values the 1982
 * manual does not explain (see the commentary on ctlr_flags_reported above).
 * Callers that want OVMX's SCC end message to be indistinguishable from the
 * VMS server's on our wire pass the observed 0xa004 / 0x0547; callers that want
 * the spec's behaviour leave them at the init() default of 0.
 * Returns 0, or -1 if srv is NULL.
 */
int scs_mscp_srv_set_ctlr_profile(struct scs_mscp_srv *srv,
                                  uint16_t ctlr_flags_reported,
                                  uint16_t ctlr_version_word);

/*
 * scs_mscp_srv_attach_fd - serve `unit` from the already-open, read-only
 * descriptor `fd`, whose first `unit_size` 512-byte blocks are the volume.
 * The descriptor is BORROWED: this module reads it with pread() and never
 * closes it.
 *
 * `unit_id` is P.UNTI and MUST be non-zero -- sec 6.12 makes a zero unit
 * identifier mean "virtually no characteristics are valid", which is not what a
 * unit we are actually serving should say about itself.
 *
 * The unit is attached READ-ONLY and UF.WPS is forced into its unit flags; see
 * design decision (2). Returns 0, or -1 on a bad argument or a full table.
 */
int scs_mscp_srv_attach_fd(struct scs_mscp_srv *srv, uint16_t unit, int fd,
                           uint32_t unit_size, uint64_t unit_id,
                           uint32_t media_id, uint32_t volume_ser);

/*
 * scs_mscp_srv_set_xfer - install the block data transfer hook. Until one is
 * installed every READ is answered with a Controller Error end message rather
 * than a Success it cannot back up; see design decision (4).
 */
int scs_mscp_srv_set_xfer(struct scs_mscp_srv *srv, scs_mscp_srv_xfer_fn fn,
                          void *ctx);

/*
 * scs_mscp_srv_find_unit - the served unit with this number, or NULL.
 * scs_mscp_srv_next_unit - the lowest-numbered served unit >= `unit`, or NULL.
 * The latter is what MD.NXU (Table A-2, sec 6.12) turns a GET UNIT STATUS into,
 * and what lets a class driver enumerate what we serve.
 */
struct scs_mscp_srv_unit *scs_mscp_srv_find_unit(struct scs_mscp_srv *srv,
                                                 uint16_t unit);
struct scs_mscp_srv_unit *scs_mscp_srv_next_unit(struct scs_mscp_srv *srv,
                                                 uint16_t unit);

/*
 * scs_mscp_srv_host_for - the per-class-driver record for this SCS Con.ID,
 * allocating one on first sight. NULL only if the table is full.
 */
struct scs_mscp_srv_host *scs_mscp_srv_host_for(struct scs_mscp_srv *srv,
                                                uint32_t conid);

/*
 * scs_mscp_srv_read_blocks - read `nblocks` 512-byte blocks starting at `lbn`
 * out of the unit's backing store into `out`. Returns the number of BYTES read,
 * or -1 on a short read, an out-of-range LBN, or an unattached unit. This is
 * the real backing-store path and is exercised directly by the unit test.
 */
long scs_mscp_srv_read_blocks(struct scs_mscp_srv_unit *u, uint32_t lbn,
                              uint32_t nblocks, uint8_t *out, size_t out_len);

/*
 * scs_mscp_srv_handle - THE RESPONDER. Decode one received MSCP command
 * (`cmd`, already parsed by scs_mscp_parse(), with `body` pointing at its
 * 12-byte-header MSCP message and `body_len` its length) and lay the end
 * message that answers it into `end`.
 *
 * `conid` identifies the class driver's connection so per-host controller state
 * (sec 3.4 "Controller-Online", sec 6.16 controller flags / host timeout) is
 * kept where the spec puts it.
 *
 * Returns the end message's LENGTH IN BYTES (a positive number), or -1 if the
 * command could not be answered at all (NULL arguments, a short output buffer,
 * or a message that is an end message rather than a command -- this module
 * answers commands, it does not answer answers).
 *
 * EVERY OTHER FAILURE IS A WELL-FORMED END MESSAGE CARRYING A REAL STATUS,
 * because that is what the protocol says failure looks like: an unknown opcode
 * gets the Invalid Command end message (sec 5.5 / Table A-1 note: "The Invalid
 * Command end message contains just OP.END"), an unknown unit gets
 * Unit-Offline, a WRITE gets Write Protected. Silence is never a response.
 */
long scs_mscp_srv_handle(struct scs_mscp_srv *srv, uint32_t conid,
                         const struct scs_mscp_view *cmd, const uint8_t *body,
                         size_t body_len, uint8_t *end, size_t end_len);

/*
 * scs_mscp_srv_build_end_frame - wrap an end-message body built by
 * scs_mscp_srv_handle() in the SCA/PPD header and SCS envelope it rides in, on
 * the connection described by `p`. `out_len` must be at least
 * SCS_MSCP_BODY_OFF + body_len + SCS_ENV_ETH_HDR_LEN.
 *
 * Returns the total frame length in bytes, or -1.
 *
 * The SCA/PPD header is the SAME labeled replay scs_mscp.c uses, with the
 * identity and sequencing fields substituted and the length words DERIVED --
 * never inherited. (Guardrail: an over-declared length word is dropped as a
 * runt in silence, because nothing in this protocol NAKs.)
 */
long scs_mscp_srv_build_end_frame(const struct scs_mscp_params *p,
                                  const uint8_t *body, size_t body_len,
                                  uint8_t *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* SCS_MSCP_SRV_H */
