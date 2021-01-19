// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/time.h>
#include <stdbool.h>

#include "host_platform.h"
#include "hyplogs.h"
#include "armtrans.h"
#include "helpers.h"
#include "guest.h"
#include "bits.h"
#include "cache.h"
#include "hvccall.h"

#define NUM_TABLES      8192
#define PT_SIZE_WORDS   512

/* Granule size constants as defined in VTCR_EL2.TG0 */
#define GRANULE_SIZE_4KB	0
#define GRANULE_SIZE_16KB	2
#define GRANULE_SIZE_64KB	1

#define L1_POFFT_MASK_4KGRANULE		0x000000003FFFFFFFUL
#define L2_POFFT_MASK_4KGRANULE		0x00000000001FFFFFUL
#define L1_BLK_OADDR_MASK_4KGRANULE	0x0000FFFFC0000000UL
#define L2_BLK_OADDR_MASK_4KGRANULE	0x0000FFFFFFE00000UL

#define TABLE_OADDR_MASK_4KGRANULE	0x0000FFFFFFFFF000UL
#define DESCR_ATTR_MASK			0xFFFF00000001FFFFUL
#define TTBR_BADDR_MASK			0x0000FFFFFFFFFFFEUL

#define MAX_PADDR			VADDR_MASK
#define MAX_VADDR			0xFFFFFFFFFFFFUL
#define TABLE_0_MASK			0xFF8000000000UL
#define TABLE_1_MASK			0x007FC0000000UL
#define TABLE_2_MASK			0x00003FE00000UL
#define TABLE_3_MASK			0x0000001FF000UL
#define PADDR_MASK			0x00FFFFFFF000UL
#define PAGE_OFF_MASK			0x000000000FFFUL

#define L0_SHIFT	(0xC + 0x9 + 0x9 + 0x9)
#define L1_SHIFT	(0xC + 0x9 + 0x9)
#define L2_SHIFT	(0xC + 0x9)
#define L3_SHIFT	(0xC)

#ifndef PLATFORM_VTCR_EL2
#define PLATFORM_VTCR_EL2 0
#endif

struct ptable
{
	uint64_t entries[PT_SIZE_WORDS];
};

struct ptable tables[NUM_TABLES] ALIGN(PAGE_SIZE) SECTION("xlat_table");
uint16_t table_props[NUM_TABLES] SECTION("xlat_table");
int debugflags = 0;

/*
 * Translation descriptor information.
 * This structure contain information
 * which changes when the translation
 * table granule size changes.
 */
typedef struct tdinfo_t
{
	uint64_t l1_blk_oa_mask;
	uint64_t l2_blk_oa_mask;
	uint64_t l1_blk_offt_mask;
	uint64_t l2_blk_offt_mask;
	uint64_t l1_blk_size;
	uint64_t l2_blk_size;
	uint64_t table_oa_mask;
} tdinfo_t;

typedef struct
{
	kvm_guest_t *guest;
	struct ptable *pgd;
	uint64_t *ptep;
	uint64_t vaddr;
	uint64_t paddr;
	uint64_t size;
	uint64_t prot;
	uint64_t type;
	uint64_t level;
	uint64_t stage;
} mblockinfo_t;

typedef enum
{
	MAPPING_ACTIVE = 1,
	MAPPING_INACTIVE = 2,
} mmap_change_type;

static uint8_t invalidate;
struct ptable *host_s2_pgd;

static struct tdinfo_t tdinfo;

void tdinfo_init(void)
{
	int granule_size;

	if (PLATFORM_VTCR_EL2 == 0)
		HYP_ABORT();

	granule_size = ((PLATFORM_VTCR_EL2 >> 14) & 3);
	switch (granule_size) {
	case GRANULE_SIZE_4KB:
		tdinfo.l1_blk_oa_mask = L1_BLK_OADDR_MASK_4KGRANULE;
		tdinfo.l2_blk_oa_mask = L2_BLK_OADDR_MASK_4KGRANULE;
		tdinfo.l1_blk_offt_mask = L1_POFFT_MASK_4KGRANULE;
		tdinfo.l2_blk_offt_mask = L2_POFFT_MASK_4KGRANULE;
		tdinfo.l1_blk_size = L1_POFFT_MASK_4KGRANULE + 1;
		tdinfo.l2_blk_size = L2_POFFT_MASK_4KGRANULE + 1;
		tdinfo.table_oa_mask = TABLE_OADDR_MASK_4KGRANULE;
		break;
	case GRANULE_SIZE_16KB:
	case GRANULE_SIZE_64KB:
	default:
		HYP_ABORT();
	break;
	}
}

static uint64_t table_oaddr(uint64_t tbl_entry)
{
	uint64_t tbl_addr = 0;

	if (bit_raised(tbl_entry, TABLE_TYPE_BIT))
		tbl_addr = tbl_entry & tdinfo.table_oa_mask;

	return tbl_addr;
}

struct ptable *alloc_table(uint32_t vm)
{
	struct ptable *table = NULL;
	int i;

	for (i = 0; i < NUM_TABLES; i++) {
		if (!bit_raised(table_props[i], VALID_TABLE_BIT)) {
			table = &tables[i];
			/* Set the owner of the table */
			table_props[i] = vm << 8;
			/* Mark the table as being used */
			bit_set(table_props[i], VALID_TABLE_BIT);
			break;
		}
	}
	return table;
}

int free_table(struct ptable *table)
{
	int res = -ENOENT, i;

	for (i = 0; i < NUM_TABLES; i++) {
		if (table == &tables[i]) {
			memset (&tables[i], 0, sizeof(struct ptable));
			table_props[i] = 0x0;
			res = 0;
		}
	}
	return res;
}

int free_guest_tables(uint64_t vmid)
{
	uint64_t owner_vm;
	int res = 0, i;

	for (i = 0; i < NUM_TABLES; i++) {
		owner_vm = table_props[i] >> 8;
		if (vmid == owner_vm) {
			memset (&tables[i], 0, sizeof(struct ptable));
			table_props[i] = 0x0;
			res += 1;
		}
	}
	return res;
}

uint64_t __pt_walk(struct ptable *tbl, uint64_t vaddr, uint64_t **ptep,
		   uint64_t *levels)
{
	struct ptable *nl = tbl;
	uint64_t noff, boff, ret, addr = 0, lvl = 0;

	if (*levels >= 4) {
		/* Level 0 */
		noff = (vaddr & TABLE_0_MASK) >> L0_SHIFT;
		if (!bit_raised(nl->entries[noff], VALID_TABLE_BIT))
			return ~0UL;
		nl = (struct ptable *)table_oaddr(nl->entries[noff]);
		if (!nl)
			return ~0UL;
	}
	lvl++;

	if (*levels >= 3) {
		/* Level 1 */
		noff = (vaddr & TABLE_1_MASK) >> L1_SHIFT;
		if (!bit_raised(nl->entries[noff], VALID_TABLE_BIT))
			return ~0UL;
		if (!bit_raised(nl->entries[noff], TABLE_TYPE_BIT))
			goto block_type;
		nl = (struct ptable *)table_oaddr(nl->entries[noff]);
		if (!nl)
			return ~0UL;
	}
	lvl++;

	/* Level 2 */
	noff = (vaddr & TABLE_2_MASK) >> L2_SHIFT;
	if (!bit_raised(nl->entries[noff], VALID_TABLE_BIT))
		return ~0UL;
	if (!bit_raised(nl->entries[noff], TABLE_TYPE_BIT))
		goto block_type;
	nl = (struct ptable *)table_oaddr(nl->entries[noff]);
	if (!nl)
		return ~0UL;
	lvl++;

	/* Level 3 */
	noff = (vaddr & TABLE_3_MASK) >> L3_SHIFT;
	if (!bit_raised(nl->entries[noff], TABLE_TYPE_BIT))
		return ~0UL;
	addr = nl->entries[noff] & PADDR_MASK;
	lvl++;

block_type:
	switch (lvl) {
	case 1:
		addr = nl->entries[noff] & tdinfo.l1_blk_oa_mask;
		boff = vaddr & tdinfo.l1_blk_offt_mask;
		break;
	case 2:
		addr = nl->entries[noff] & tdinfo.l2_blk_oa_mask;
		boff = vaddr & tdinfo.l2_blk_offt_mask;
		break;
	default:
		addr = addr & ~PAGE_OFF_MASK;
		boff = vaddr & PAGE_OFF_MASK;
		break;
	}
	*levels = lvl;

	ret = addr | boff;

	if (ptep)
		*ptep = &nl->entries[noff];

	return ret;
}

uint64_t pt_walk(struct ptable *tbl, uint64_t vaddr, uint64_t **ptep,
		 uint64_t levels)
{
	uint64_t level = levels;

	return __pt_walk(tbl, vaddr, ptep, &level);
}

void print_mappings(uint32_t vmid, uint64_t stage, uint64_t vaddr, size_t sz)
{
	uint64_t start_vaddr = 0, end_vaddr = 0, start_addr = 0, perms = 0;
	uint64_t addr = 0, size = 0;
	uint64_t operms = ~0UL;
	uint64_t oaddr = ~0UL;
	kvm_guest_t *guest;
	struct ptable *pgd;
	uint64_t *pte;

	guest = get_guest(vmid);
	if (!guest) {
		ERROR("No such guest %u?\n", vmid);
		return;
	}
	switch (stage) {
	case STAGE2:
		pgd = guest->s2_pgd;
		break;
	case STAGE1:
		pgd = guest->s1_pgd;
		break;
	default:
		return;
	}
	LOG("VMID %u pgd %p mappings %p - %p\n", vmid,
						(void *)pgd,
						(void *)vaddr,
						(void *)(vaddr + sz));
	LOG("vaddr\t\tpaddr\t\tsize\t\tprot\n");

	while (vaddr <= sz) {
		pte = NULL;
		addr = pt_walk(pgd, vaddr, &pte, 4);
		/*
		 * See if the block is mapped
		 */
		if (!pte || (vaddr && !addr)) {
			operms = ~0UL;
			vaddr += PAGE_SIZE;
			continue;
		}
		/*
		 * Reset to new mapping
		 */
		perms = *pte & PROT_MASK_STAGE2;
		if (operms == ~0UL)
			operms = perms;
		/*
		 * Seek to the end of the new mapping
		 */
		if ((addr == (oaddr + PAGE_SIZE)) && (perms == operms)) {
			operms = perms;
			oaddr = addr;
			vaddr += PAGE_SIZE;
			end_vaddr = vaddr;
			continue;
		}
		/*
		 * Print it
		 */
		if (!end_vaddr)
			end_vaddr = vaddr;
		size = end_vaddr - start_vaddr;

		LOG("0x%012lx\t0x%012lx\t0x%012lx\t0x%012lx\n",
		     start_vaddr, start_addr, size, perms);

		start_vaddr = vaddr;
		start_addr = addr;

		operms = perms;
		oaddr = addr;
		vaddr += PAGE_SIZE;
		end_vaddr = vaddr;
	}
	/* Last entry, if there is one  */
	if (!end_vaddr)
		return;

	size = end_vaddr - start_vaddr;
	LOG("0x%012lx\t0x%012lx\t0x%012lx\t0x%012lx\n",
	     start_vaddr, start_addr, size, perms);
}

void print_table(struct ptable *addr)
{
	uint64_t *ptr = (uint64_t *)addr;
	int i, z = 0, b = 0;

	for (i = 0; i < PT_SIZE_WORDS; i++) {
		if (!ptr[i])
			continue;
		if (!b) {
			printf("Table 0x%lx\n", addr);
			b = 1;
		}
		printf("%03d:0x%014lx ", i, ptr[i]);
		z++;
		if ((z % 4) == 0) {
			printf("\n");
			z = 0;
		}
	}
	if (b && ((z % 4) != 0))
		printf("\n");
}

void print_tables(uint64_t vmid)
{
	kvm_guest_t *guest;
	int i;

	if (!debugflags)
		return;

	guest = get_guest(vmid);
	if (!guest) {
		ERROR("No such guest %u?\n", vmid);
		return;
	}
	/*
	 * Guest pgd may belong to host accounting and it may be
	 * freed separately. Dump it just in case.
	 */
	printf("Page table data for vmid %lu ::::\n", vmid);
	print_table(guest->s2_pgd);

	for (i = 0; i < NUM_TABLES; i++) {
		if (bit_raised(table_props[i], VALID_TABLE_BIT)) {
			if ((table_props[i] >> 8) == vmid)
				print_table(&tables[i]);
                }
	}
}

/*
 * Return true if block mapping for the given virtual address is found
 * and block info is populated.
 * Return false if no block mapping is found for the address.
 */
bool get_block_info(const uint64_t addr, mblockinfo_t *block)
{
	uint64_t paddr, offt;

	if (!block->guest || ((block->guest->s2_pgd != block->pgd) &&
		(block->guest->s1_pgd != block->pgd)))
		HYP_ABORT();

	block->level = block->guest->table_levels;
	paddr = __pt_walk(block->pgd, addr, &block->ptep, &block->level);
	if (paddr == ~0UL)
		return false;

	if (bit_raised(*block->ptep, TABLE_TYPE_BIT))
		return false;

	if (block->level == 1) {
		block->paddr = *block->ptep & tdinfo.l1_blk_oa_mask;
		block->size = tdinfo.l1_blk_size;
	} else {
		block->paddr = *block->ptep & tdinfo.l2_blk_oa_mask;
		block->size = tdinfo.l2_blk_size;
	}

	if (block->stage == STAGE1) {
		block->prot = *block->ptep & PROT_MASK_STAGE1;
		block->type = (*block->ptep & TYPE_MASK_STAGE1) >> 2;
	} else {
		block->prot = *block->ptep & PROT_MASK_STAGE2;
		block->type = (*block->ptep & TYPE_MASK_STAGE2) >> 2;
	}

	offt = paddr - block->paddr;
	block->vaddr = addr - offt;

	return true;
}

static int mmap_addr(struct ptable *pgd, uint64_t vaddr, uint64_t paddr,
		     uint64_t range_size, uint64_t prot, uint64_t type,
		     uint64_t levels, uint32_t vmid)
{
	struct ptable *tp, *nl;
	uint64_t noff;
	int res = 0;

	tp = pgd;
	if (!tp || (vaddr > MAX_VADDR) || (paddr > MAX_PADDR))
		return -EINVAL;

	if (levels >= 4) {
		/*
		 * Level 0
		 */
		noff = (vaddr & TABLE_0_MASK) >> L0_SHIFT;
		nl = (struct ptable *)table_oaddr(tp->entries[noff]);
		if (!nl) {
			nl = alloc_table(vmid);
			if (!nl) {
				res = -ENOSPC;
				goto out_error;
			}

			tp->entries[noff] = (uint64_t)nl;
			bit_set(tp->entries[noff], VALID_TABLE_BIT);
			bit_set(tp->entries[noff], TABLE_TYPE_BIT);
		}
		tp = nl;
	}

	if (levels >= 3) {
		/*
		 * Level 1
		 */
		noff = (vaddr & TABLE_1_MASK) >> L1_SHIFT;
		nl = (struct ptable *)table_oaddr(tp->entries[noff]);
		if (range_size == (1 << L1_SHIFT)) {
			/*
			 * If this is a remap, verify there is no existing
			 * table we are going to overwrite.
			 */
			if (bit_raised(tp->entries[noff], TABLE_TYPE_BIT)) {
				free_table((struct ptable *)
					   table_oaddr(tp->entries[noff]));
			}
			/* Clear all the fields other than output address */
			tp->entries[noff] = paddr & tdinfo.l1_blk_oa_mask;
			goto out_finalize;
		}
		if (!nl) {
			nl = alloc_table(vmid);
			if (!nl) {
				res = -ENOSPC;
				goto out_error;
			}

			tp->entries[noff] = (uint64_t)nl;
		}
		/* Since these can be remaps of blocks, assume nothing */
		tp->entries[noff] &= tdinfo.table_oa_mask;
		bit_set(tp->entries[noff], VALID_TABLE_BIT);
		bit_set(tp->entries[noff], TABLE_TYPE_BIT);
		tp = nl;
	}

	if (levels >= 2) {
		/*
		 * Level 2
		 */
		noff = (vaddr & TABLE_2_MASK) >> L2_SHIFT;
		if (range_size == (1 << L2_SHIFT)) {
			if (bit_raised(tp->entries[noff], TABLE_TYPE_BIT)) {
				free_table((struct ptable *)
					   table_oaddr(tp->entries[noff]));
			}
			tp->entries[noff] = paddr & tdinfo.l2_blk_oa_mask;
			goto out_finalize;
		}
		nl = (struct ptable *)table_oaddr(tp->entries[noff]);
		if (!nl) {
			nl = alloc_table(vmid);
			if (!nl) {
				res = -ENOSPC;
				goto out_error;
			}

			tp->entries[noff] = (uint64_t)nl;
		}
		tp->entries[noff] &= tdinfo.table_oa_mask;
		bit_set(tp->entries[noff], VALID_TABLE_BIT);
		bit_set(tp->entries[noff], TABLE_TYPE_BIT);
		tp = nl;
	}

	/*
	 * Level 3, the page descriptor.
	 */
	noff = (vaddr & TABLE_3_MASK) >> L3_SHIFT;
	tp->entries[noff] = paddr & tdinfo.table_oa_mask;
	if (tp->entries[noff])
		bit_set(tp->entries[noff], TABLE_TYPE_BIT);

out_finalize:
	if (!tp->entries[noff] || (type == INVALID_MEMORY)) {
		tp->entries[noff] = 0x0;
		res = 0;
		goto out_error;
	}
	/* Permissions and shareability of the area */
	tp->entries[noff] |= prot;

	/* Type of memory we refer to */
	tp->entries[noff] |= type << 2;

	/* Validify it */
	bit_set(tp->entries[noff], VALID_TABLE_BIT);
	bit_set(tp->entries[noff], AF_BIT);

out_error:
	dsb();
	return res;
}

/*
 * Find the largest block size that can be mapped for this address
 * and range size.
 */
static uint64_t get_block_size(uint64_t vaddr, size_t length)
{
	uint64_t block_sz;

	if ((vaddr & tdinfo.l1_blk_offt_mask) == 0) {
		if (length >= tdinfo.l1_blk_size) {
			block_sz = tdinfo.l1_blk_size;
			goto out;
		}
	}
	if ((vaddr & tdinfo.l2_blk_offt_mask) == 0) {
		if (length >= tdinfo.l2_blk_size) {
			block_sz = tdinfo.l2_blk_size;
			goto out;
		}
	}
	block_sz = PAGE_SIZE;

out:
	return block_sz;
}

static int get_mapping_type(struct ptable *pgd)
{
	if (pgd == (struct ptable *)read_reg(TTBR0_EL2))
		return MAPPING_ACTIVE;
	if (pgd == (struct ptable *)read_reg(VTTBR_EL2))
		return MAPPING_ACTIVE;
	return MAPPING_INACTIVE;
}

static void invalidate_va(struct ptable *pgd, uint64_t vaddr)
{
	if (!invalidate)
		return;

	dsb();
	if (pgd == (struct ptable *)read_reg(TTBR0_EL2))
		tlbi_el2_va(vaddr);
	if (pgd == (struct ptable *)read_reg(VTTBR_EL2))
		tlbi_el1_ipa(vaddr);
	dsb();
	isb();
}

int __mmap_range(struct ptable *pgd, uint64_t vaddr, uint64_t paddr,
		 size_t length, uint64_t prot, uint64_t type,
		 uint64_t levels, uint32_t vmid)
{
	uint64_t blk_sz, new_blk_sz, tlength;
	mmap_change_type ctype;
	int res;

	/* Return zero size mappings explicitly here.*/
	if (length <= 0)
		return 0;

	if (type > INVALID_MEMORY)
		return -EINVAL;

	ctype = get_mapping_type(pgd);
	vaddr = vaddr & VADDR_MASK;
	tlength = ROUND_UP(length, 0x1000);

	blk_sz = 0;
	while (tlength > 0) {
		spinner();

		new_blk_sz = get_block_size(vaddr, tlength);
		if (blk_sz != new_blk_sz)
			blk_sz = new_blk_sz;

		/*
		 * If this a currently active hyp mode stage-1 OR active
		 * stage-2 mapping change, do full break-before-make cycle.
		 */
		if (invalidate && paddr && (ctype == MAPPING_ACTIVE)) {
			res = mmap_addr(pgd, vaddr, 0x0, blk_sz, prot,
					INVALID_MEMORY, levels, vmid);
			if (res)
				return res;
			invalidate_va(pgd, vaddr);
		}

		res = mmap_addr(pgd, vaddr, paddr, blk_sz, prot, type, levels,
				vmid);
		if (res)
			return res;

		if (invalidate && (ctype == MAPPING_ACTIVE))
			invalidate_va(pgd, vaddr);

		vaddr += blk_sz;
		if (paddr)
			paddr += blk_sz;
		tlength -= blk_sz;
	}

	if (invalidate)
		tlbialle1is();
	isb();

	return 0;
}

int __block_remap(uint64_t vaddr, size_t len, mblockinfo_t *block,
		  uint64_t paddr, uint64_t prot, uint64_t type,
		  uint64_t pgd_levels)
{
	uint64_t tvaddr, tpaddr, levels, bsize;
	struct ptable *tbl;
	int res = 0;
	size_t rlen, tlen, mlen;
	uint32_t vmid = block->guest->vmid;
	bool hit;

	if (len <= 0)
		goto out_done;

	/*
	 * For the sake of faster boot up it is left
	 * to the machine initialization code responsibility
	 * to make sure there is no overlapping mappings in
	 * initial configuration.
	 */
	if (!machine_init_ready()) {
		res = __mmap_range(block->pgd, vaddr, paddr, len,
				   prot, type, pgd_levels, vmid);
		goto out_done;
	}

	tvaddr = vaddr;
	tpaddr = paddr;

	/*
	 * Map the new range and check for overlapping
	 * block mappings.
	 */
	rlen = len;
	while (rlen > 0) {
		hit = get_block_info(tvaddr, block);
		if (hit) {
			/*
			 * If we are at the block boundary and the
			 * remaining length is equal (or larger)
			 * to the size of the block we found:
			 * We don't need to split the block.
			 * We may remap the whole block instead.
			 */
			if (((tvaddr & (block->size - 1)) == 0) &&
			    (rlen >= block->size)) {
				hit = false;
				bsize = block->size;
				mlen = block->size;
			}
		} else {
			/*
			 * There was no block mapped at tvaddr. We can
			 * map all until the next possibly mapped (level 2)
			 * block boundary.
			 */
			mlen = tvaddr & tdinfo.l2_blk_offt_mask;
			bsize = tdinfo.l2_blk_size;
		}
		if (hit) {
			invalidate = 0;

			/*
			 * Get a table entry into which we start building our
			 * new mapping. This will replace the block entry we
			 * found.
			 */
			tbl = alloc_table(vmid);
			if (!tbl)
				HYP_ABORT();

			/*
			 * Starting level of the new mapping.
			 * If this was a level 1 block we need
			 * to map it to blocks and pages starting
			 * at level 2.
			 */
			if (block->level == 1)
				levels = 2;
			else
				levels = 1;

			/*
			 * Create mapping for the address range covering the
			 * original block range before the vaddr.
			 */
			tlen = tvaddr - block->vaddr;
			res = __mmap_range(tbl, block->vaddr, block->paddr,
					   tlen, block->prot, block->type,
					   levels, vmid);
			if (res)
				HYP_ABORT();
			LOG("head v:0x%lx p:0x%lx l:%lu\n",
			    block->vaddr, block->paddr, tlen);
			block->vaddr += tlen;
			block->paddr += tlen;

			/* Size left within this block. */
			tlen = block->size - tlen;
			if (rlen < tlen) {
				mlen = rlen;
				tlen -= rlen;
				rlen = 0;
			} else {
				mlen = tlen;
				/* Map may reach the next block.*/
				rlen -= tlen;
				tlen = 0;
			}

			/* New range mapping */
			res = __mmap_range(tbl, tvaddr, tpaddr, mlen, prot,
					type, levels, vmid);
			if (res)
				HYP_ABORT();
			LOG("map v:0x%lx l:%lu\n", tvaddr, mlen);
			tvaddr += mlen;
			tpaddr += mlen;
			block->vaddr += mlen;
			block->paddr += mlen;

			/*
			 * Create mapping for the address range covering the
			 * original block range after the vaddr + rlen.
			 */
			res = __mmap_range(tbl, block->vaddr, block->paddr,
					   tlen, block->prot, block->type,
					   levels, vmid);
			if (res)
				HYP_ABORT();

			LOG("tail v:0x%lx p:0x%lx l:%lu\n",
			    block->vaddr, block->paddr, tlen);

			/* Activate our Brave New mapping */
			*block->ptep = (uint64_t)tbl;
			bit_set(*block->ptep, VALID_TABLE_BIT);
			bit_set(*block->ptep, TABLE_TYPE_BIT);

			invalidate = 1;
			/* Invalidate */
			dsbishst();
			tlbivmallis();
			dsbish();
			isb();
		} else {
			if (mlen == 0)
				mlen = bsize;
			if (mlen > rlen)
				mlen = rlen;
			res = __mmap_range(block->pgd, tvaddr, tpaddr, mlen,
					   prot, type, pgd_levels, vmid);
			if (res)
				HYP_ABORT();
			/*LOG("map nohit v:0x%lx l:%lu\n", tvaddr, mlen);*/
			tvaddr += mlen;
			tpaddr += mlen;
			rlen -= mlen;
		}
	}

out_done:
	return res;
}

static struct ptable *host_pgd(uint64_t stage)
{
	kvm_guest_t *guest;

	guest = get_guest(HOST_VMID);
	switch (stage) {
	case STAGE2:
		return guest->s2_pgd;
	case STAGE1:
		return guest->s1_pgd;
	default:
		return NULL;
	}
}

int mmap_range(struct ptable *pgd, uint64_t stage, uint64_t vaddr,
	       uint64_t paddr, size_t length, uint64_t prot, uint64_t type)
{
	mblockinfo_t block;

	memset(&block, 0, sizeof(block));

	if (!pgd) {
		block.guest = get_guest(HOST_VMID);
		switch (stage) {
		case STAGE2:
			/* Only allow unmap or 1:1 mapping for the host */
			if ((vaddr != paddr) && paddr) {
				ERROR("invalid host st2 mapping 0x%lx - 0x%lx\n",
				      vaddr, paddr);
				return -EINVAL;
			}
			block.pgd = block.guest->s2_pgd;
			prot &= PROT_MASK_STAGE2;
			break;
		case STAGE1:
			block.pgd = block.guest->s1_pgd;
			prot &= PROT_MASK_STAGE1;
			break;
		default:
			return -EINVAL;
		}
	} else {
		switch (stage) {
		case STAGE2:
			block.guest = get_guest_by_s2pgd(pgd);
			block.pgd = pgd;
			prot &= PROT_MASK_STAGE2;
			break;
		case STAGE1:
			block.guest = get_guest_by_s1pgd(pgd);
			block.pgd = pgd;
			prot &= PROT_MASK_STAGE1;
			break;
		default:
			return -EINVAL;
		}
	}

	block.stage = stage;

	return __block_remap(vaddr, length, &block,
			     paddr, prot, type, TABLE_LEVELS);
}

int unmap_range(struct ptable *pgd, uint64_t stage, uint64_t vaddr,
		size_t length)
{
	mblockinfo_t block;

	if (!pgd) {
		block.guest = get_guest(HOST_VMID);
		block.pgd = host_pgd(stage);
	} else {
		switch (stage) {
		case STAGE2:
			block.guest = get_guest_by_s2pgd(pgd);
			block.pgd = pgd;
		case STAGE1:
			block.guest = get_guest_by_s1pgd(pgd);
			block.pgd = pgd;
		default:
			return -EINVAL;
		}
	}

	if (!block.pgd || !block.guest || length < PAGE_SIZE)
		return -EINVAL;

	block.stage = stage;

	return __block_remap(vaddr, length, &block,
			     0, 0, INVALID_MEMORY, TABLE_LEVELS);
}

void table_init(void)
{
	kvm_guest_t *host;

	/* Clean up everything */
	memset(tables, 0, sizeof(tables));
	__flush_dcache_area((void *)tables, sizeof(tables));

	memset(table_props, 0, sizeof(table_props));
	__flush_dcache_area((void *)table_props, sizeof(table_props));
	isb();

	/* Init host side tables */
	host = get_guest(HOST_VMID);
	host->s1_pgd = alloc_table(HOST_VMID);
	host->s2_pgd = alloc_table(HOST_VMID);
	host_s2_pgd = host->s2_pgd;

	LOG("host info: vmid %x, s1 pgd 0x%lx, s2 pgd 0x%lx\n",
	    HOST_VMID, (uint64_t)host->s1_pgd, (uint64_t)host->s2_pgd);
}

void enable_mmu(void)
{
	uint64_t hcr, sctlr;
	kvm_guest_t *host;

	hcr = read_reg(HCR_EL2);
	tlbialle1is();
	tlbialle2is();
	dsbish();
	isb();
	/*
	 * 0: device_sorder
	 * 1: device_order
	 * 2: device_gre
	 * 3: normal, outer/inner no-cache
	 * 4: normal, wback persistent
	 * 5: normal, wthrough persistent
	 * 6: --
	 * 7: --
	 */
	write_reg(MAIR_EL2, 0x0000bbff440c0400);

	if (PLATFORM_VTCR_EL2 != 0)
		write_reg(VTCR_EL2, PLATFORM_VTCR_EL2);

	if (PLATFORM_TCR_EL2 != 0)
		write_reg(TCR_EL2, PLATFORM_TCR_EL2);

	host = get_guest(HOST_VMID);
	write_reg(TTBR0_EL2, (uint64_t)host->s1_pgd);
	write_reg(VTTBR_EL2, (uint64_t)host->s2_pgd);
	set_current_vmid(HOST_VMID);
	host->table_levels = TABLE_LEVELS;

	bit_set(hcr, HCR_VM_BIT);
	bit_drop(hcr, HCR_NV_BIT);
	bit_set(hcr, HCR_NV2_BIT);
	write_reg(HCR_EL2, hcr);

	dsb();
	isb();

	sctlr = read_reg(SCTLR_EL2);
	bit_set(sctlr, SCTLR_MMU);
	bit_set(sctlr, SCTLR_A);
	bit_set(sctlr, SCTLR_C);
	write_reg(SCTLR_EL2, sctlr);

	/*
	 * Make sure our mmu enable has been registered
	 * before proceeding any further.
	 */
	isb();
	tlbialle1is();
	tlbialle2is();
	tlbivmall();
	dsbish();
	isb();

	invalidate = 1;
	update_guest_state(guest_running);
}
