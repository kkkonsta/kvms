// SPDX-License-Identifier: GPL-2.0-only
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "helpers.h"
#include "mtree.h"
#include "guest.h"
#include "armtrans.h"
#include "hvccall.h"
#include "mm.h"
#include "bits.h"

#include "host_platform.h"
#include "include/generated/asm-offsets.h"

#ifndef KVM_ARCH
#define KVM_ARCH 0
#define KVM_ARCH_VMID 0
#define KVM_ARCH_PGD 0
#endif

/*
 * FIXME: calculate vmid offset
 * properly in asm_offsets.c
 */
#ifndef KVM_ARCH_VMID_OFFT
#define KVM_ARCH_VMID_OFFT 0
#endif

#define _KVM_GET_ARCH(x) ((char *)x + KVM_ARCH)
#define _KVM_GET_VMID(x) (_KVM_GET_ARCH((char *)x) + \
			  KVM_ARCH_VMID + KVM_ARCH_VMID_OFFT)

#define KVM_GET_VMID(x) (*(uint32_t *)_KVM_GET_VMID(x))
#define KVM_GET_PGD_PTR(x) (uint64_t *)(_KVM_GET_ARCH((char *)x) + KVM_ARCH_PGD)

static kvm_guest_t guests[MAX_GUESTS];

kvm_guest_t *get_free_guest()
{
	int i;

	for (i = 0; i < MAX_GUESTS; i++) {
		if (!guests[i].s2_pgd)
			return &guests[i];
	}
	return NULL;
}

kvm_guest_t *get_guest(uint64_t vmid)
{
	kvm_guest_t *guest = NULL;
	int i;

	if (vmid != HOST_VMID) {
		for (i = 0; i < MAX_GUESTS; i++) {
			if (guests[i].kvm &&
			   (vmid == (uint64_t)KVM_GET_VMID(guests[i].kvm))) {
				guests[i].vmid = vmid;
				guest = &guests[i];
				goto out;
			}
		}
	} else {
		for (i = 0; i < MAX_GUESTS; i++) {
			if (guests[i].vmid == HOST_VMID) {
				guest = &guests[i];
				goto out;
			}
		}
		guest = get_free_guest();
		guest->vmid = HOST_VMID;
	}
out:
	return guest;
}

int update_guest_state(guest_state_t state)
{
	kvm_guest_t *guest;
	uint64_t vmid;

	vmid = get_current_vmid();
	guest = get_guest(vmid);
	if (!guest)
		return -ENOENT;

	guest->state = state;
	return 0;
}

guest_state_t get_guest_state(uint64_t vmid)
{
	kvm_guest_t *guest;

	guest = get_guest(vmid);
	if (!guest)
		return guest_invalid;

	return guest->state;
}

int init_guest(void *kvm)
{
	kvm_guest_t *guest = NULL;
	uint64_t *pgd;
	int i;

	if (!kvm)
		return -EINVAL;

	kvm = kern_hyp_va(kvm);
	for (i = 0; i < MAX_GUESTS; i++) {
		if (guests[i].kvm == kvm) {
			guest = &guests[i];
			break;
		}
	}

	if (!guest) {
		guest = get_free_guest();
		if (!guest)
			return -ENOSPC;

		/*
		 * VMID is not allocated yet, so use host
		 * tracking and a separate free.
		 */
		guest->s2_pgd = alloc_table(HOST_VMID);
		if (!guest->s2_pgd) {
			free_guest(guest);
			return -ENOMEM;
		}
	}

	/*
	 * FIXME: the real thing to do here is to protect the kernel
	 * writable shared blobs, like the kvm.
	 */
	pgd = KVM_GET_PGD_PTR(kvm);
	*pgd = (uint64_t)guest->s2_pgd;
	guest->kvm = kvm;
	guest->table_levels = TABLE_LEVELS;

	/* Save the current VM process stage1 PGD */
	guest->s1_pgd = (struct ptable *)read_reg(TTBR0_EL1);
#if DEBUGDUMP
	print_mappings(0, STAGE2, SZ_1G, SZ_1G*5);
#endif

	dsb();
	isb();
	return 0;
}

/* NOTE; KVM is hyp addr */

kvm_guest_t *get_guest_by_kvm(void *kvm)
{
	kvm_guest_t *guest = NULL;
	int i, rc = 0;

retry:
	for (i = 0; i < MAX_GUESTS; i++) {
		if (guests[i].kvm == kvm) {
			guest = &guests[i];
			break;
		}
	}
	if (!guest) {
		i = init_guest(kvm);
		if (i)
			return NULL;
		rc += 1;
		if (rc < 2)
			goto retry;
	}
	return guest;
}

kvm_guest_t *get_guest_by_s1pgd(struct ptable *pgd)
{
	kvm_guest_t *guest = NULL;
	int i;

	for (i = 0; i < MAX_GUESTS; i++) {
		if (guests[i].s1_pgd == pgd) {
			guest = &guests[i];
			break;
		}
	}
	return guest;
}

kvm_guest_t *get_guest_by_s2pgd(struct ptable *pgd)
{
	kvm_guest_t *guest = NULL;
	int i;

	for (i = 0; i < MAX_GUESTS; i++) {
		if (guests[i].s2_pgd == pgd) {
			guest = &guests[i];
			break;
		}
	}
	return guest;
}

int guest_map_range(kvm_guest_t *guest, uint64_t vaddr, uint64_t paddr,
		    uint64_t len, uint64_t prot)
{
	uint64_t page_vaddr, page_paddr, taddr, *pte;
	uint64_t newtype, maptype, mapprot, mc = 0;
	int res;

	if (!guest || !vaddr || !paddr || (len % PAGE_SIZE)) {
		res = -EINVAL;
		goto out_error;
	}
	/*
	 * Verify that the range is within the guest boundary.
	 */
	res = is_range_valid(vaddr, len, guest->slots);
	if (!res) {
		ERROR("vmid %x attempting to map invalid range 0x%lx - 0x%lx\n",
		      guest->vmid, vaddr, vaddr + len);
		res = -EINVAL;
		goto out_error;
	}

	newtype = (prot & TYPE_MASK_STAGE2);

	/*
	 * Do we know about this area?
	 */
	page_vaddr = vaddr;
	page_paddr = paddr;
	while (page_vaddr < (vaddr + len)) {
		/*
		 * If it's already mapped, bail out. This is not secure,
		 * so for all remaps force a unmap first so that we can
		 * measure the existing content.
		 */
		pte = NULL;
		taddr = pt_walk(guest->s2_pgd, page_vaddr, &pte,
				TABLE_LEVELS);
		if ((taddr != ~0UL) && (taddr != page_paddr)) {
			ERROR("vmid %x 0x%lx already mapped: 0x%lx != 0x%lx\n",
			      guest->vmid, (uint64_t)page_vaddr,
			      taddr, page_paddr);
			/*
			 * TODO: Find and fix the issue causing this error
			 * during guest boot. Memory related to this issue
			 * is initially mapped by do_xor_speed calling
			 * xor_8regs_2 (uses pages got by __get_free_pages).
			 * Later on the guest boot the same physical pages
			 * get mapped without unmapping them first and we
			 * end up here.
			 *
			res = -EPERM;
			goto out_error;
			*/
		}
		/*
		 * Track identical existing mappings
		 */
		if (pte) {
			maptype = (*pte & TYPE_MASK_STAGE2);
			mapprot = (*pte & PROT_MASK_STAGE2);
			if ((taddr == page_paddr) && (maptype == newtype) &&
			    (mapprot == prot)) {
				mc++;
				continue;
			}
		}
		/*
		 * If it wasn't mapped and we are mapping it back,
		 * verify that the content is still the same.
		 */
		res = verify_range(guest, page_vaddr, page_paddr, PAGE_SIZE);
		if (res == -EINVAL)
			goto out_error;

		page_vaddr += PAGE_SIZE;
		page_paddr += PAGE_SIZE;
	}
	/*
	 * If we had already mapped this area as per the request,
	 * this is probably smp related trap we already served. Don't
	 * hinder the guest progress by remapping again and doing
	 * the full break-before-make cycle.
	 */
	if (len == (mc * PAGE_SIZE))
		return 0;

	/*
	 * Request the MMU to tell us if this was touched, if it can.
	 */
	bit_set(prot, DBM_BIT);

	res = mmap_range(guest->s2_pgd, STAGE2, vaddr, paddr, len, prot,
			 KERNEL_MATTR);
	if (!res)
		res = remove_host_range(paddr, len);

out_error:
	return res;
}

int guest_unmap_range(kvm_guest_t *guest, uint64_t vaddr, uint64_t len,
		      bool measure)
{
	uint64_t paddr, map_addr;
	uint64_t *pte;
	int res = -EINVAL, pc = 0;

	if (!guest || !vaddr || (len % PAGE_SIZE)) {
		res = 0xF0F0;
		goto out_error;
	}

	map_addr = vaddr;
	while (map_addr < (vaddr + len)) {
		paddr = pt_walk(guest->s2_pgd, map_addr, &pte,
				TABLE_LEVELS);
		if (paddr == ~0UL)
			goto do_loop;
		/*
		 * If the vm dirty data is never allowed to leak, don't set
		 * up a swap. Normal clean / file backed page reclaim will
		 * work and the dirty data can't leak to the swapfile.
		 *
		 * If you have a swap, things will still work to the extent
		 * that we verify that the data comes back from the swap
		 * intact.
		 */
		if (measure) {
			/*
			 * This is a mmu notifier chain call and the blob may
			 * get swapped out or freed. Take a measurement to
			 * make sure it does not change while out.
			 */
			res = add_range_info(guest, vaddr, paddr, PAGE_SIZE);
			if (res)
				ERROR("add_range_info(): guest %d %lld:%d\n",
				      guest->vmid, vaddr, res);
		}
		/*
		 * Do not leak guest data
		 */
		memset((void *)paddr, 0, PAGE_SIZE);
		/*
		 * Detach the page from the guest
		 */
		res = unmap_range(guest->s2_pgd, STAGE2, vaddr, PAGE_SIZE);
		if (res)
			ERROR("unmap_range(): %lld:%d\n", vaddr, res);
		/*
		 * Give it back to the host
		 */
		res = restore_host_range(paddr, PAGE_SIZE);
		if (res)
			HYP_ABORT();

		pc += 1;
do_loop:
		map_addr += PAGE_SIZE;
		if (pc == 0xFFFF)
			ERROR("Unmap page counter overflow");
	}

out_error:
	/*
	 * If it ended with an error, append info on how many
	 * pages were actually unmapped.
	 */
	if (res)
		res |= (pc << 16);

	return res;

}

int free_guest(void *kvm)
{
	kvm_guest_t *guest = NULL;
	int i, res;

	if (!kvm)
		return -EINVAL;

	kvm = kern_hyp_va(kvm);
	for (i = 0; i < MAX_GUESTS; i++) {
		if (guests[i].kvm == kvm) {
			guest = &guests[i];
			break;
		}
	}
	if (!guest)
		return 0;

	if (guest->vmid) {
		res = restore_host_mappings(guest);
		if (res)
			return res;

		free_guest_tables(guest->vmid);
		free_table(guest->s2_pgd);
	}

	memset (guest, 0, sizeof(*guest));
#if DEBUGDUMP
	print_mappings(0, STAGE2, SZ_1G, SZ_1G*5);
#endif
	dsb();
	isb();

	return 0;
}

int update_memslot(void *kvm, kvm_memslot *slot,
		   kvm_userspace_memory_region *reg)
{
	kvm_guest_t *guest;
	uint64_t addr, size;

	if (!kvm || !slot || !reg)
		return -EINVAL;

	kvm = kern_hyp_va(kvm);
	slot = kern_hyp_va(slot);
	reg = kern_hyp_va(reg);

	if (slot->npages > 0x100000)
		return -EINVAL;

	guest = get_guest_by_kvm(kvm);
	if (!guest)
		return 0;

	if (guest->sn > KVM_MEM_SLOTS_NUM)
		return -EINVAL;

	addr = fn_to_addr(slot->base_gfn);
	size = slot->npages * PAGE_SIZE;

	/* Check dupes */
	if (is_range_valid(addr, size, &guest->slots[0]))
		return 0;

	memcpy(&guest->slots[guest->sn].region, reg, sizeof(*reg));
	memcpy(&guest->slots[guest->sn].slot, slot, sizeof(*slot));

	LOG("guest 0x%lx slot 0x%lx - 0x%lx\n", kvm, addr, addr + size);
	guest->sn++;

	dsb();
	isb();

	return 0;
}

int guest_user_copy(uint64_t dest, uint64_t src, uint64_t count)
{
	uint64_t ttbr0_el1 = (read_reg(TTBR0_EL1) & TTBR_BADDR_MASK);
	uint64_t ttbr1_el1 = (read_reg(TTBR1_EL1) & TTBR_BADDR_MASK);
	uint64_t usr_addr, dest_pgd, src_pgd;
	kvm_guest_t *guest = NULL;

	/* Check if we have such guest */
	guest = get_guest_by_s1pgd((struct ptable *)ttbr0_el1);
	if (guest == NULL)
		return -ENOENT;

	if (guest->vmid == HOST_VMID)
		return -ENOENT;

	/*
	 * Set the guest user address and page tables to use.
	 * ttbr1_el1 contain the host kernel view of the memory.
	 * ttbr0_el1 contain the guest user space view of the memory.
	 */
	if ((dest & KERNEL_MAP)) {
		usr_addr = src;
		dest_pgd = ttbr1_el1;
		src_pgd = ttbr0_el1;
	} else {
		usr_addr = dest;
		dest_pgd = ttbr0_el1;
		src_pgd = ttbr1_el1;
	}

	/* Check that the guest address is within guest boundaries */
	if (!is_range_valid_uaddr(usr_addr, count, &guest->slots[0]))
		return -EINVAL;

	return user_copy(dest, src, count, dest_pgd, src_pgd);
}

static int compfunc(const void *v1, const void *v2)
{
	kvm_page_data *val1 = (kvm_page_data *)v1;
	kvm_page_data *val2 = (kvm_page_data *)v2;

	return (val1->phys_addr - val2->phys_addr);
}

kvm_page_data *get_range_info(kvm_guest_t *guest, uint64_t ipa)
{
	kvm_page_data key, *res;

	if (!guest)
		return NULL;

	key.phys_addr = ipa;
	res = bsearch(&key, guest->hyp_page_data, guest->pd_index,
		      sizeof(key), compfunc);

	return res;
}

int add_range_info(kvm_guest_t *guest, uint64_t ipa, uint64_t addr,
		   uint64_t len)
{
	kvm_page_data *res;
	int ret;
	bool s = false;

	if (!guest || !ipa || !addr || !len || len % PAGE_SIZE)
		return -EINVAL;

	res = get_range_info(guest, ipa);
	if (res)
		goto use_old;

	if (guest->pd_index == MAX_PAGING_BLOCKS - 1)
		return -ENOSPC;

	s = true;
	res = &guest->hyp_page_data[guest->pd_index];
	res->phys_addr = addr;
	guest->pd_index += 1;

use_old:
	res->vmid = guest->vmid;
	res->len = len;
	ret = calc_hash(res->sha256, (void *)addr, len);
	if (ret) {
		memset(res->sha256, 0, 32);
		res->vmid = 0;
	}
	if (s)
		qsort(guest->hyp_page_data, guest->pd_index, sizeof(kvm_page_data),
		      compfunc);
	dsb();
	isb();

	return ret;
}

void free_range_info(kvm_guest_t *guest, uint64_t ipa)
{
	kvm_page_data *res;

	res = get_range_info(guest, ipa);
	if (!res)
		return;

	res->vmid = 0;
	memset(res->sha256, 0, 32);
	dsb();
	isb();
}

int verify_range(kvm_guest_t *guest, uint64_t ipa, uint64_t addr, uint64_t len)
{
	kvm_page_data *res;
	uint8_t sha256[32];
	int ret;

	res = get_range_info(guest, ipa);
	if (!res)
		return -ENOENT;

	if (res->vmid != guest->vmid)
		return -EFAULT;

	ret = calc_hash(sha256, (void *)addr, len);
	if (ret)
		return -EFAULT;

	ret = memcmp(sha256, res->sha256, 32);
	if (ret != 0)
		return -EINVAL;

	return 0;
}
