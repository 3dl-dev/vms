/*
 * ovmx_image.h — OVMX shareable-image symbol-vector format.
 *
 * The shared contract between LINK.EXE (the OVMX/VMS linker, src/vmslink) and
 * IMGACT.EXE (SYS$IMGACT, src/imgact). Beads vms-9dd / vms-8d5, pillar vms-ade.
 *
 * ---------------------------------------------------------------------------
 * CLEAN-ROOM NOTE (CLAUDE.md rule 8): This layout is an *OVMX-original design
 * choice*, NOT a reproduction of any VSI/HPE on-disk structure. Public VMS docs
 * specify the symbol-vector *semantics* (universal symbols bound by fixed
 * position/offset; x86-64 entry = quadword pair, I64 = single quadword; append-
 * only + retire-in-place upward compatibility; GSMATCH major/minor match rules)
 * but do NOT publish the byte-level ELF section that carries the vector on
 * x86-64/I64. Where VMS is silent, OVMX defines its own representation and
 * labels it as such. See docs/design-link-native-toolchain.md §5.6.
 * ---------------------------------------------------------------------------
 *
 * Model: an OVMX shareable image is an ELF ET_DYN object that additionally
 * carries a `.vms$sv` section. That section holds the SYMBOL VECTOR: an ordered
 * table of universal-symbol entries at fixed indices. A consumer image binds to
 * a producer's universal symbol by (producer-soname, vector INDEX) — never by
 * name at run time — exactly as VMS binds by vector position. IMGACT resolves
 * each import by reading producer.vector[index].value (relocated to a run-time
 * address by a standard ELF R_*_RELATIVE reloc that LINK.EXE emits over the
 * value field) and patching the consumer's reference.
 */
#ifndef OVMX_IMAGE_H
#define OVMX_IMAGE_H

#include <stdint.h>

/* Section that carries the symbol vector in an OVMX shareable image. */
#define OVMX_SV_SECTION   ".vms$sv"
/* Section that carries a consumer image's universal-symbol imports. */
#define OVMX_IMP_SECTION  ".vms$imp"
/* Section that carries an image's WEAK-by-name imports: references LINK.EXE
 * could bind to no --use'd producer at link time, but which IMGACT resolves by
 * NAME against whatever producers happen to be loaded at activation (found ->
 * bind; absent -> the cell stays 0, ELF weak-undef semantics). This is how a
 * lower-layer producer (LIBVMS$SHR) reaches a universal exported by a
 * HIGHER-layer producer it must not --use at build time because that would
 * invert the layering into a build cycle (LIBVMSRMS$SHR --use's LIBVMS$SHR, so
 * LIBVMS$SHR cannot --use LIBVMSRMS$SHR to import sys$open/$get/$connect/$close
 * by index). By-name activation binding is exactly how VMS resolves a
 * shareable's inter-image references; the fixed (producer,index) `.vms$imp`
 * path cannot express a cyclic edge. (vms-5f0) */
#define OVMX_WIMP_SECTION ".vms$wimp"
/* Section that lists image-relative slots needing +load_bias at activation. */
#define OVMX_REL_SECTION  ".vms$rel"
/* Section that lists the image's TLSDESC entries for the activator to complete. */
#define OVMX_TLS_SECTION  ".vms$tls"
/* Section that names the image's DWARF .eh_frame block + libgcc __register_frame
 * so the activator can register the frames before .init_array runs (vms-70d). */
#define OVMX_EHF_SECTION  ".vms$ehf"
/* Section that carries the VMS TRANSFER-ADDRESS ARRAY + an activation flavor
 * (vms-f60d). This is the image-header side of the Alpha calling standard's
 * image-startup model: the ordered list of routines the activator invokes at
 * startup (LIB$INITIALIZE handlers, then the main transfer address), plus a
 * flavor selecting HOW IMGACT enters that transfer address. Its presence with
 * flavor==OVMX_ACT_VMS_STD is how IMGACT recognizes a VMS-standard image (an
 * `alpha-dec-vms` GCC-port `.EXE`, whose crt0 __main expects a six-argument
 * standard call). ABSENCE => flavor OVMX_ACT_SYSV => today's tail-jump path,
 * so every image OVMX ships now is byte-unchanged (zero regression).
 *
 * OVMX-ORIGINAL carrier (CLAUDE.md Rule 8): the transfer-array + startup
 * SEMANTICS are public (OpenVMS Calling Standard / Programming Concepts
 * Manual), but no public byte-level x86/Alpha section layout exists for it, so
 * this section format is OVMX's own -- same posture as .vms$sv/.vms$rel/
 * .vms$tls above, and NOT presented as a VMS-authentic on-disk structure. */
#define OVMX_XFER_SECTION ".vms$xfer"

#define OVMX_SV_MAGIC     0x31565356u  /* "VSV1" little-endian */
#define OVMX_IMP_MAGIC    0x31504d49u  /* "IMP1" little-endian */
#define OVMX_WIMP_MAGIC   0x314d4957u  /* "WIM1" little-endian */
#define OVMX_REL_MAGIC    0x314c4552u  /* "REL1" little-endian */
#define OVMX_TLS_MAGIC    0x31534c54u  /* "TLS1" little-endian */
#define OVMX_EHF_MAGIC    0x31464845u  /* "EHF1" little-endian */
#define OVMX_XFER_MAGIC   0x31465358u  /* "XSF1" little-endian */

/* GSMATCH match-control (public VMS semantics; values are OVMX-internal). */
enum ovmx_gsmatch {
    OVMX_GSMATCH_ALWAYS = 0,  /* any version accepted (major/minor ignored)   */
    OVMX_GSMATCH_EQUAL  = 1,  /* image minor must equal linked minor          */
    OVMX_GSMATCH_LEQUAL = 2,  /* image minor >= linked minor (same/newer OK)  */
};

/* Universal-symbol kind (public VMS SYMBOL_VECTOR keywords). */
enum ovmx_sv_kind {
    OVMX_SV_PROCEDURE = 1,  /* value = code entry-point address              */
    OVMX_SV_DATA      = 2,  /* value = address of a data location            */
    OVMX_SV_RETIRED   = 3,  /* PRIVATE_* — slot preserved, not publicly bound */
};

/*
 * `.vms$sv` layout:  header, then `count` entries, then a name string blob.
 * Names exist for diagnostics/dumping only; run-time binding is by INDEX.
 *
 * OVMX design choice (aarch64/x86-64): a single quadword `value` per entry.
 * Public docs give x86-64 a quadword *pair*; the second quadword there carries
 * no payload for a pure code/data address on our targets (no GP/descriptor), so
 * OVMX uses one quadword and records the kind separately. Labeled OVMX-original.
 */
struct ovmx_sv_header {
    uint32_t magic;         /* OVMX_SV_MAGIC                                  */
    uint32_t count;         /* number of vector entries (including RETIRED)   */
    uint32_t gsmatch_kind;  /* enum ovmx_gsmatch                             */
    uint32_t gsmatch_major; /* GSMATCH major id                              */
    uint32_t gsmatch_minor; /* GSMATCH minor id                              */
    uint32_t names_off;     /* byte offset (from header) to the name blob    */
    uint32_t names_size;    /* size of the name blob                         */
    uint32_t reserved;
    /* struct ovmx_sv_entry entries[count]; */
    /* char names[names_size]; */
};

struct ovmx_sv_entry {
    uint64_t value;         /* image-relative at link; run-time addr after a  */
                            /* R_*_RELATIVE reloc LINK.EXE emits on this field */
    uint32_t kind;          /* enum ovmx_sv_kind                             */
    uint32_t name_off;      /* offset into the name blob (diagnostics only)   */
};

/*
 * `.vms$imp` layout (consumer side): header, then `count` import records, then
 * a producer-soname string blob. Each record binds one local reference to a
 * (producer, vector-index) pair; IMGACT writes the resolved address to
 * `patch_off` within the importing image.
 */
struct ovmx_imp_header {
    uint32_t magic;         /* OVMX_IMP_MAGIC                                */
    uint32_t count;         /* number of import records                      */
    uint32_t names_off;     /* offset to producer-soname blob               */
    uint32_t names_size;
    /* struct ovmx_imp_entry entries[count]; */
    /* char names[names_size]; */
};

struct ovmx_imp_entry {
    uint32_t producer_off;  /* offset into soname blob: producer image name  */
    uint32_t sv_index;      /* index into the producer's symbol vector        */
    uint64_t patch_off;     /* image-relative slot to receive the resolved    */
                            /* address at activation (a GOT-like cell)        */
    uint32_t req_major;     /* producer GSMATCH major this consumer linked to */
    uint32_t req_minor;     /* producer GSMATCH minor this consumer linked to */
};

/*
 * `.vms$wimp` layout (weak-by-name imports): header, then `count` records, then
 * a symbol-NAME string blob (NOT producer sonames — the producer is unknown at
 * link time and chosen by IMGACT at activation). Each record names one universal
 * to resolve by NAME across the loaded producer set; found -> write the resolved
 * address to `patch_off`; absent -> leave the cell as LINK.EXE left it (0), the
 * ELF weak-undef result. patch_off is the same import-GOT cell a `.vms$imp`
 * record would name, so a weak import shares the PLT/import-GOT machinery — only
 * its binding is deferred to a name lookup instead of a (producer,index) pair.
 */
struct ovmx_wimp_header {
    uint32_t magic;         /* OVMX_WIMP_MAGIC                               */
    uint32_t count;         /* number of weak-import records                 */
    uint32_t names_off;     /* offset (from header) to the symbol-name blob  */
    uint32_t names_size;
    /* struct ovmx_wimp_entry entries[count]; */
    /* char names[names_size]; */
};

struct ovmx_wimp_entry {
    uint32_t name_off;      /* offset into the name blob: universal name     */
    uint32_t reserved;      /* 0 (alignment / future flags)                  */
    uint64_t patch_off;     /* image-relative import-GOT cell to receive the  */
                            /* by-name-resolved address at activation         */
};

/*
 * `.vms$rel` layout: header then `count` image-relative offsets (u64 each). Each
 * offset names an 8-byte slot inside the image (a synthesized GOT cell, or a
 * pointer-valued data word) that LINK.EXE filled with an image-relative address.
 * At activation IMGACT adds the image's load bias to each such slot — the
 * VMS-native equivalent of the ELF R_*_RELATIVE relocation, without a
 * PT_DYNAMIC/DT_RELA. OVMX-original design (CLAUDE.md rule 8): public VMS docs
 * describe self-relative image fixups but publish no byte-level table format.
 */
struct ovmx_rel_header {
    uint32_t magic;         /* OVMX_REL_MAGIC                                 */
    uint32_t count;         /* number of image-relative slot offsets          */
    /* uint64_t offsets[count]; */
};

/*
 * `.vms$tls` layout: header then `count` image-relative offsets (u64 each), one
 * per synthesized TLSDESC entry. A TLSDESC entry is two quadwords:
 *   entry[0] = resolver function address  (LINK leaves 0; IMGACT fills with the
 *              interpreter's __tlsdesc_static)
 *   entry[1] = TP-relative offset of the variable (LINK pre-fills the
 *              MODULE-relative offset = symbol value + addend; IMGACT adds the
 *              module's assigned TLS block offset)
 * This replaces the ELF R_AARCH64_TLSDESC dynamic relocation on the symbol-
 * vector activation path, which has no PT_DYNAMIC. OVMX-original design
 * (CLAUDE.md rule 8): the TLSDESC *semantics* are the public AArch64 ELF ABI;
 * this section is OVMX's own carrier for the activator to complete them.
 */
struct ovmx_tls_header {
    uint32_t magic;         /* OVMX_TLS_MAGIC                                 */
    uint32_t count;         /* number of TLSDESC entries                     */
    /* uint64_t entry_off[count]; image-relative offset of each 2-word entry  */
};

/*
 * `.vms$ehf` layout (vms-70d): a single descriptor naming the image's
 * DWARF unwinder frame table and the libgcc entry that registers it.
 *
 * A normal static C++ link relies on crtbeginT.o's frame_dummy() ctor calling
 * __register_frame_info(__EH_FRAME_BEGIN__) to register the TU's .eh_frame with
 * libgcc's registered-object list, so _Unwind_Find_FDE finds FDEs there and
 * never falls to its dl_iterate_phdr path. An OVMX symbol-vector image has no
 * working dl_iterate_phdr (IMGACT's custom activation never populates musl's
 * link_map), and LINK.EXE lays .eh_frame out from concatenated whole-archive
 * members that DON'T carry a single contiguous begin/terminator crtbegin/crtend
 * pair would provide. So LINK.EXE instead places all .eh_frame input sections
 * CONTIGUOUSLY, appends its own 4-byte-zero FDE terminator, and records here
 * the image-relative address of that block's start and of the image's own
 * whole-archived __register_frame. IMGACT reads this, biases both by the load
 * base, and calls __register_frame(eh_frame_begin) BEFORE running .init_array
 * (so libstdc++'s eh_alloc emergency-pool ctor and any later throw find their
 * FDEs in the registry). Emitted ONLY when the image both has a non-empty
 * .eh_frame and defines __register_frame (i.e. it whole-archived libgcc_eh /
 * libstdc++); absent for a pure-C image, where IMGACT skips registration.
 * OVMX-original carrier (CLAUDE.md rule 8): the __register_frame semantics are
 * libgcc's public unwinder ABI; this section is OVMX's activator hook for them.
 */
struct ovmx_ehf_desc {
    uint32_t magic;             /* OVMX_EHF_MAGIC                             */
    uint32_t reserved;
    uint64_t eh_frame_begin;    /* image-relative start of the .eh_frame block */
    uint64_t register_frame;    /* image-relative addr of libgcc __register_frame */
};

/*
 * How IMGACT enters an image's transfer address. LINK.EXE records the flavor
 * in the image's `.vms$xfer` section (vms-f60d).
 *
 *   OVMX_ACT_SYSV    - current OVMX crt0 / legacy images: IMGACT tail-jumps to
 *                      the entry with the Linux/SysV initial stack in place
 *                      (argc/argv/envp/auxv), registers cleared. This is the
 *                      behavior for EVERY image with no `.vms$xfer` section, so
 *                      absence == this value == today's path, ZERO regression.
 *   OVMX_ACT_VMS_STD - a VMS-standard image (the `alpha-dec-vms` GCC port's
 *                      `.EXE`): IMGACT issues a genuine Alpha standard call to
 *                      the transfer address with the six-argument VMS
 *                      image-activation context, then captures the returned
 *                      VMS condition value (it RETURNS to IMGACT, unlike SYSV).
 */
enum ovmx_act_flavor {
    OVMX_ACT_SYSV    = 0,
    OVMX_ACT_VMS_STD = 1,
};

/*
 * `.vms$xfer` layout: header, then `count` image-relative transfer addresses
 * (u64 each). The LAST entry is the MAIN transfer address (the crt0 __main /
 * ELF$TFRADR); any earlier entries are LIB$INITIALIZE handlers invoked before
 * it (none emitted at first light -- count starts at 1). At activation IMGACT
 * biases each entry by the image load base. For OVMX_ACT_VMS_STD the resolved
 * last entry is the procedure value (PV/PDSC) IMGACT places in R27 and whose
 * offset-8 entry code it standard-calls.
 *
 * OVMX-ORIGINAL design (CLAUDE.md Rule 8): see OVMX_XFER_SECTION above.
 */
struct ovmx_xfer_header {
    uint32_t magic;         /* OVMX_XFER_MAGIC                                */
    uint32_t flavor;        /* enum ovmx_act_flavor                          */
    uint32_t count;         /* number of transfer entries (>= 1)             */
    uint32_t reserved;      /* 0                                             */
    /* uint64_t entry_off[count]; image-relative transfer addresses,         */
    /* last == main transfer address (__main); earlier == LIB$INITIALIZE     */
};

#endif /* OVMX_IMAGE_H */
