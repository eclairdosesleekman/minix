#ifndef __SMP_X86_H__
#define __SMP_X86_H__

#include "arch_proto.h" /* K_STACK_SIZE */

#define MAX_NR_INTERRUPT_ENTRIES	128

#ifndef __ASSEMBLY__

/*
 * Resolve the logical CPU number during SMP bootstrap or diagnostics.
 * CPUID.01H:EBX[31:24] reports the local APIC ID; arch_smp.c maps that
 * hardware ID to the logical CPU number assigned from the ACPI MADT.
 * This slow resolver is not used by the CPU-local hot path.
 */
unsigned int x86_current_cpu_id(void);

/*
 * Resolve the current logical CPU ID from the local APIC ID and the ACPI
 * MADT map.  This accessor avoids relying on a global register variable:
 * the LLVM 3.6 x86_64 backend used by the MINIX cross toolchain cannot lower
 * global register variables such as asm("r15").
 */
#define cpuid	x86_current_cpu_id()

/*
 * switch_k_stack() jumps to its continuation; it does not push a return
 * address as CALL would.  Leave one word below the two per-CPU words
 * reserved by tss_init(), so a C continuation starts with the SysV AMD64
 * ABI stack alignment (RSP % 16 == 8).
 */
#define X86_STACK_SWITCH_SP(cpu)	\
	((char *)get_k_stack_top(cpu) - (3 * sizeof(reg_t)))
/*
 * If APIC or SMP is disabled in the boot monitor, finish booting on the
 * legacy PIC.  As in the normal SMP path, move off the EFI bootstrap stack
 * before entering bsp_finish_booting(): timer initialization enables
 * interrupts and the interrupt path needs the dedicated per-CPU stack (and
 * its CPU-id slot, initialized by tss_init()).
 */
#define smp_single_cpu_fallback() do {		\
	  bsp_cpu_id = 0;			\
	  ncpus = 1;				\
	  tss_init(0, get_k_stack_top(0));	\
	  setup_sysenter_syscall();		\
	  switch_k_stack(X86_STACK_SWITCH_SP(0), \
			  bsp_finish_booting);		\
} while(0)

extern unsigned char cpuid2apicid[CONFIG_MAX_CPUS];

#define barrier()	do { mfence(); } while(0)

#endif

#endif /* __SMP_X86_H__ */
