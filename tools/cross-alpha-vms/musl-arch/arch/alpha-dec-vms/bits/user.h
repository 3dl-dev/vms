/*
 * user.h - ptrace register views, Alpha.
 * OVMX alpha-dec-vms musl port (vms-960).
 *
 * Alpha ELF core note layout: 33 integer slots (r0-r30, pc, and the "unique"
 * slot) plus the FP register set. RUNG-1 note: ptrace/core is not exercised
 * (syscalls stubbed); provided for header completeness, reconciled at GAP3.
 */
#define ELF_NGREG 33
typedef unsigned long elf_greg_t, elf_gregset_t[ELF_NGREG];

#define ELF_NFPREG 32
typedef double elf_fpreg_t;
typedef elf_fpreg_t elf_fpregset_t[ELF_NFPREG];

struct user {
	unsigned long regs[ELF_NGREG];
	unsigned long fpregs[ELF_NFPREG];
	unsigned long u_tsize;
	unsigned long u_dsize;
	unsigned long u_ssize;
	unsigned long start_code;
	unsigned long start_data;
	unsigned long start_stack;
	long int signal;
	unsigned long u_ar0;
	unsigned long magic;
	char u_comm[32];
};
