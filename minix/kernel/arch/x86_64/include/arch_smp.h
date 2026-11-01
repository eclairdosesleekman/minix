#ifndef __SMP_X86_H__
#define __SMP_X86_H__

#include "arch_proto.h" /* K_STACK_SIZE */

#define MAX_NR_INTERRUPT_ENTRIES	128

#ifndef __ASSEMBLY__

/*
 * Returns the current cpu id.  The id is stored as a reg_t at
 * kernel_stack_top - sizeof(reg_t) by tss_init() (see protect.c).  Use
 * vir_bytes (64-bit) for the address arithmetic so the kernel stack VA at
 * 0xFFFFFFFF80xxxxxx is not truncated, and read it as a reg_t so [-1] lands
 * on the full 8-byte slot rather than its high (zero) half.
 */
#define cpuid	(((reg_t *)(((vir_bytes)get_stack_frame() + (K_STACK_SIZE - 1)) \
						& ~(K_STACK_SIZE - 1)))[-1])
/*
 * If APIC or SMP is disabled in the boot monitor, finish booting on the
 * legacy PIC.  As in the normal SMP path, move off the EFI bootstrap stack
 * before entering bsp_finish_booting(): timer initialization enables
 * interrupts and the interrupt path needs the dedicated per-CPU stack (and
 * its CPU-id slot, initialized by tss_init()).
 */
#define smp_single_cpu_fallback() do {		\
	  tss_init(0, get_k_stack_top(0));	\
	  setup_sysenter_syscall();		\
	  bsp_cpu_id = 0;			\
	  ncpus = 1;				\
	  switch_k_stack((char *)get_k_stack_top(0) - (2 * sizeof(reg_t)), \
			  bsp_finish_booting);		\
} while(0)

extern unsigned char cpuid2apicid[CONFIG_MAX_CPUS];

#define barrier()	do { mfence(); } while(0)

#endif

#endif /* __SMP_X86_H__ */
