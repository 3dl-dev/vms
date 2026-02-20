/*
 * HWRPBDEF.H - VMS Hardware Restart Parameter Block Definitions
 *
 * OpenVMX compatibility layer - Defines the HWRPB structure and related
 * constants for the Hardware Restart Parameter Block on Alpha systems.
 *
 * The HWRPB is loaded into a known physical page by the console firmware
 * at boot time (typically at physical address 0x2000 on Alpha) and is
 * mapped into virtual memory by the OS for system software use.
 *
 * Reference: OpenVMS Alpha Architecture Reference Manual
 *            Alpha Architecture Reference Manual
 */

#ifndef __HWRPBDEF_H
#define __HWRPBDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * struct _hwrpb — Hardware Restart Parameter Block
 *
 * This is a simplified stub with the fields used by corpus programs.
 * The full HWRPB has many additional fields.
 * ================================================================ */

struct _hwrpb {
    uint64_t hwrpb$pq_base;     /* Physical base address of HWRPB */
    uint64_t hwrpb$iq_ident;    /* Hardware identification string ("HWRPB") */
    uint64_t hwrpb$q_rev_level; /* Revision level */
    uint64_t hwrpb$q_size;      /* Size of HWRPB (bytes) */
    uint64_t hwrpb$q_cpuid;     /* Primary CPU identifier */
    uint64_t hwrpb$q_pagesize;  /* System page size (bytes) */
    uint64_t hwrpb$q_pa_bits;   /* Number of physical address bits */
    uint64_t hwrpb$q_maxasn;    /* Maximum valid ASN */
    uint64_t hwrpb$q_serialnum; /* System serial number */
    uint64_t hwrpb$q_systype;   /* System type */
    uint64_t hwrpb$q_sysvar;    /* System variation */
    uint64_t hwrpb$q_sysrev;    /* System revision */
    uint64_t hwrpb$q_intr_freq; /* Interval timer frequency */
    uint64_t hwrpb$q_cc_freq;   /* Cycle counter frequency */
    uint64_t hwrpb$q_vptb;      /* Virtual page table base */
    uint64_t hwrpb$q_rsvd_arch; /* Reserved for architecture */
    uint64_t hwrpb$q_tbhint;    /* Translation buffer hint */
    uint64_t hwrpb$q_ncpus;     /* Number of CPU slots */
    uint64_t hwrpb$q_cpuoff;    /* Offset to first per-CPU slot */
    uint64_t hwrpb$q_updtime;   /* Update time */
};

typedef struct _hwrpb HWRPB;

/* ================================================================
 * EXE$GPL_HWRPB_L — System global pointing to the mapped HWRPB
 *
 * In VMS, this is a system global that holds the virtual address
 * of the HWRPB.  On OVMX this is declared extern for compilation.
 * ================================================================ */

extern struct _hwrpb *EXE$GPL_HWRPB_L;

#ifdef __cplusplus
}
#endif

#endif /* __HWRPBDEF_H */
