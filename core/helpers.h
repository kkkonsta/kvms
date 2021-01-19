/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __HELPERS_H__
#define __HELPERS_H__

#include <stdint.h>
#include <stdio.h>

#include "hyplogs.h"
#include "commondefines.h"

#define read_reg(r)                                                            \
	__extension__({                                                        \
		uint64_t value;                                                \
		__asm__ __volatile__("mrs	%0, " string(r)                \
				     : "=r"(value));                           \
		value;                                                         \
	})

#define read_gpreg(r)                                                          \
	__extension__({                                                        \
		uint64_t value;                                                \
		__asm__ __volatile__("mov	%0, " string(r)                \
				     : "=r"(value));                           \
		value;                                                         \
	})

#define write_reg(r, v)                                                        \
	do {                                                                   \
		uint64_t value = (uint64_t)v;                                  \
		__asm__ __volatile__("msr	" string(r) ", %0"             \
				     :                                         \
				     : "r"(value));                            \
	} while (0);

#define ats1e1r(va)                                                            \
	({                                                                     \
		uint64_t value;                                                \
		__asm__ __volatile__("at	s1e1r, %[vaddr]\n"             \
				     "mrs	%[paddr], PAR_EL1\n"           \
				     : [paddr] "=r"(value)                     \
				     : [vaddr] "r"(va)                         \
				     :);                                       \
		value;                                                         \
	})

/*
 * x20 recycle is a product of working around a gcc optimizer bug,
 * apogies.
 */

#define tlbi_el1_va(va)                                                        \
	do {                                                                   \
		__asm__ __volatile__("mov	x20, %[vaddr]\n"               \
				     "lsr	%[vaddr], %[vaddr], #12\n"     \
				     "tlbi	vae1is, %[vaddr]\n"            \
				     "mov	%[vaddr], x20\n"               \
				     :                                         \
				     : [vaddr] "r"(va)                         \
				     : "memory", "x20");                       \
	} while (0);

#define tlbi_el1_ipa(va)                                                       \
	do {                                                                   \
		__asm__ __volatile__("mov	x20, %[vaddr]\n"               \
				     "lsr	%[vaddr], %[vaddr], #12\n"     \
				     "tlbi	ipas2le1is, %[vaddr]\n"        \
				     "mov	%[vaddr], x20\n"               \
				     :                                         \
				     : [vaddr] "r"(va)                         \
				     : "memory", "x20");                       \
	} while (0);

#define tlbi_el2_va(va)                                                        \
	do {                                                                   \
		__asm__ __volatile__("mov	x20, %[vaddr]\n"               \
				     "lsr	%[vaddr], %[vaddr], #12\n"     \
				     "tlbi	vae2is, %[vaddr]\n"            \
				     "mov	%[vaddr], x20\n"               \
				     :                                         \
				     : [vaddr] "r"(va)                         \
				     : "memory", "x20");                       \
	} while (0);

#define get_current_vmid() (read_reg(VTTBR_EL2) >> 48)
#define set_current_vmid(x) write_reg(VTTBR_EL2, (read_reg(VTTBR_EL2) | ((uint64_t)x << 48)))

static inline uint64_t smp_processor_id()
{
	uint64_t value;

	__asm__ __volatile__("mrs	%[v], mpidr_el1\n"
			     "and	%[v], %[v], #0xff00\n"
			     "lsr	%[v], %[v], #8\n"
			     : [v] "=r"(value)
			     :
			     :);

	return value;
}

#define tlbialle1() __asm__ __volatile__("tlbi	alle1\n" : : : "memory");

#define tlbialle1is() __asm__ __volatile__("tlbi	alle1is\n" : : : "memory");

#define tlbialle2() __asm__ __volatile__("tlbi	alle2\n" : : : "memory");

#define tlbialle2is() __asm__ __volatile__("tlbi	alle2is\n" : : : "memory");

#define tlbivmall() __asm__ __volatile__("tlbi	vmalls12e1\n" : : : "memory");

#define tlbivmallis() __asm__ __volatile__("tlbi	vmalls12e1is\n" : : : "memory");

#define dmb() __asm__ __volatile__("dmb	sy\n" : : : "memory");

#define dsb() __asm__ __volatile__("dsb	sy\n" : : : "memory");

#define dsbishst() __asm__ __volatile__("dsb	ishst\n" : : : "memory");

#define dsbish() __asm__ __volatile__("dsb	ish\n" : : : "memory");

#define isb() __asm__ __volatile__("isb	sy\n" : : : "memory");

#define smc() __asm__ __volatile__("smc	#0\n" : : :);

#define eret() __asm__ __volatile__("eret\n" : : :);

#define wfe() __asm__ __volatile__("wfe\n" : : :);

#define wfi() __asm__ __volatile__("wfi\n" : : :);

#define per_cpu_ptr(ptr, cpu)                                                  \
	((typeof(ptr))((char *)(ptr) + (4 * sizeof(long)) * cpu))

typedef enum { cold_reset = 0, warm_reset } reset_type;

extern void __inval_dcache_area(void *addr, size_t len);

#endif