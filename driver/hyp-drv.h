/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2021 Digital14 Ltd.
 *
 * Authors:
 * Konsta Karsisto <konsta.karsisto@gmail.com>
 *
 * File: hyp-drv.h
 *	Hypervisor call module for userspace
 */

#ifndef __HYP_DRV__
#define __HYP_DRV__

struct hypdrv_mem_region {
	u64 start;
	u64 end;
	u64 prot;
};

struct log_frag {
	u64 frag;
};

#define KERNEL_LOCK	1
#define KERNEL_MMAP	2
#define KERNEL_WRITE	3
#define READ_LOG	4

#define HYPDRV_IOCTL_BASE 0xDE
#define HYPDRV_KERNEL_LOCK _IO(HYPDRV_IOCTL_BASE, 1)
#define HYPDRV_KERNEL_MMAP _IOW(HYPDRV_IOCTL_BASE, 2, struct hypdrv_mem_region)
#define HYPDRV_KERNEL_WRITE _IOW(HYPDRV_IOCTL_BASE, 3, struct hypdrv_mem_region)
#define HYPDRV_READ_LOG _IOR(HYPDRV_IOCTL_BASE, 4, struct log_frag)

#define _XN(A, B)	(A<<54|B<<53)
#define _SH(A, B)	(A<<9|B<<8)
#define _S2AP(A, B)	(A<<7|B<<6)
#define HYPDRV_KERNEL_EXEC	(_XN(1UL, 1UL)|_SH(1UL, 1UL)|_S2AP(0UL, 1UL))
#define HYPDRV_PAGE_KERNEL	(_XN(1UL, 0UL)|_SH(1UL, 1UL)|_S2AP(1UL, 1UL))
#define HYPDRV_PAGE_VDSO	(_XN(0UL, 0UL)|_SH(1UL, 1UL)|_S2AP(1UL, 1UL))
#define HYPDRV_PAGE_KERNEL_RO	(_XN(1UL, 0UL)|_SH(1UL, 1UL)|_S2AP(0UL, 1UL))

#define s2_inone	0x1 /* Inner Non-cacheable */
#define s2_iwt		0x2 /* Inner Write-Through Cacheable */
#define s2_iwb		0xf /* WAS: 0x3 ??? Inner Write-Back Cacheable */

#endif // __HYP_DRV__