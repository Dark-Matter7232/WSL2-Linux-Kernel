// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/kvm/lib/x86_64/processor.c
 *
 * Copyright (C) 2018, Google LLC.
 */

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"

#ifndef NUM_INTERRUPTS
#define NUM_INTERRUPTS 256
#endif

#define DEFAULT_CODE_SELECTOR 0x8
#define DEFAULT_DATA_SELECTOR 0x10

vm_vaddr_t exception_handlers;

static void regs_dump(FILE *stream, struct kvm_regs *regs, uint8_t indent)
{
	fprintf(stream, "%*srax: 0x%.16llx rbx: 0x%.16llx "
		"rcx: 0x%.16llx rdx: 0x%.16llx\n",
		indent, "",
		regs->rax, regs->rbx, regs->rcx, regs->rdx);
	fprintf(stream, "%*srsi: 0x%.16llx rdi: 0x%.16llx "
		"rsp: 0x%.16llx rbp: 0x%.16llx\n",
		indent, "",
		regs->rsi, regs->rdi, regs->rsp, regs->rbp);
	fprintf(stream, "%*sr8:  0x%.16llx r9:  0x%.16llx "
		"r10: 0x%.16llx r11: 0x%.16llx\n",
		indent, "",
		regs->r8, regs->r9, regs->r10, regs->r11);
	fprintf(stream, "%*sr12: 0x%.16llx r13: 0x%.16llx "
		"r14: 0x%.16llx r15: 0x%.16llx\n",
		indent, "",
		regs->r12, regs->r13, regs->r14, regs->r15);
	fprintf(stream, "%*srip: 0x%.16llx rfl: 0x%.16llx\n",
		indent, "",
		regs->rip, regs->rflags);
}

static void segment_dump(FILE *stream, struct kvm_segment *segment,
			 uint8_t indent)
{
	fprintf(stream, "%*sbase: 0x%.16llx limit: 0x%.8x "
		"selector: 0x%.4x type: 0x%.2x\n",
		indent, "", segment->base, segment->limit,
		segment->selector, segment->type);
	fprintf(stream, "%*spresent: 0x%.2x dpl: 0x%.2x "
		"db: 0x%.2x s: 0x%.2x l: 0x%.2x\n",
		indent, "", segment->present, segment->dpl,
		segment->db, segment->s, segment->l);
	fprintf(stream, "%*sg: 0x%.2x avl: 0x%.2x "
		"unusable: 0x%.2x padding: 0x%.2x\n",
		indent, "", segment->g, segment->avl,
		segment->unusable, segment->padding);
}

static void dtable_dump(FILE *stream, struct kvm_dtable *dtable,
			uint8_t indent)
{
	fprintf(stream, "%*sbase: 0x%.16llx limit: 0x%.4x "
		"padding: 0x%.4x 0x%.4x 0x%.4x\n",
		indent, "", dtable->base, dtable->limit,
		dtable->padding[0], dtable->padding[1], dtable->padding[2]);
}

static void sregs_dump(FILE *stream, struct kvm_sregs *sregs, uint8_t indent)
{
	unsigned int i;

	fprintf(stream, "%*scs:\n", indent, "");
	segment_dump(stream, &sregs->cs, indent + 2);
	fprintf(stream, "%*sds:\n", indent, "");
	segment_dump(stream, &sregs->ds, indent + 2);
	fprintf(stream, "%*ses:\n", indent, "");
	segment_dump(stream, &sregs->es, indent + 2);
	fprintf(stream, "%*sfs:\n", indent, "");
	segment_dump(stream, &sregs->fs, indent + 2);
	fprintf(stream, "%*sgs:\n", indent, "");
	segment_dump(stream, &sregs->gs, indent + 2);
	fprintf(stream, "%*sss:\n", indent, "");
	segment_dump(stream, &sregs->ss, indent + 2);
	fprintf(stream, "%*str:\n", indent, "");
	segment_dump(stream, &sregs->tr, indent + 2);
	fprintf(stream, "%*sldt:\n", indent, "");
	segment_dump(stream, &sregs->ldt, indent + 2);

	fprintf(stream, "%*sgdt:\n", indent, "");
	dtable_dump(stream, &sregs->gdt, indent + 2);
	fprintf(stream, "%*sidt:\n", indent, "");
	dtable_dump(stream, &sregs->idt, indent + 2);

	fprintf(stream, "%*scr0: 0x%.16llx cr2: 0x%.16llx "
		"cr3: 0x%.16llx cr4: 0x%.16llx\n",
		indent, "",
		sregs->cr0, sregs->cr2, sregs->cr3, sregs->cr4);
	fprintf(stream, "%*scr8: 0x%.16llx efer: 0x%.16llx "
		"apic_base: 0x%.16llx\n",
		indent, "",
		sregs->cr8, sregs->efer, sregs->apic_base);

	fprintf(stream, "%*sinterrupt_bitmap:\n", indent, "");
	for (i = 0; i < (KVM_NR_INTERRUPTS + 63) / 64; i++) {
		fprintf(stream, "%*s%.16llx\n", indent + 2, "",
			sregs->interrupt_bitmap[i]);
	}
}

void virt_arch_pgd_alloc(struct kvm_vm *vm)
{
	TEST_ASSERT(vm->mode == VM_MODE_PXXV48_4K, "Attempt to use "
		"unknown or unsupported guest mode, mode: 0x%x", vm->mode);

	/* If needed, create page map l4 table. */
	if (!vm->pgd_created) {
		vm->pgd = vm_alloc_page_table(vm);
		vm->pgd_created = true;
	}
}

static void *virt_get_pte(struct kvm_vm *vm, uint64_t pt_pfn, uint64_t vaddr,
			  int level)
{
	uint64_t *page_table = addr_gpa2hva(vm, pt_pfn << vm->page_shift);
	int index = (vaddr >> PG_LEVEL_SHIFT(level)) & 0x1ffu;

	return &page_table[index];
}

static uint64_t *virt_create_upper_pte(struct kvm_vm *vm,
				       uint64_t pt_pfn,
				       uint64_t vaddr,
				       uint64_t paddr,
				       int current_level,
				       int target_level)
{
	uint64_t *pte = virt_get_pte(vm, pt_pfn, vaddr, current_level);

	if (!(*pte & PTE_PRESENT_MASK)) {
		*pte = PTE_PRESENT_MASK | PTE_WRITABLE_MASK;
		if (current_level == target_level)
			*pte |= PTE_LARGE_MASK | (paddr & PHYSICAL_PAGE_MASK);
		else
			*pte |= vm_alloc_page_table(vm) & PHYSICAL_PAGE_MASK;
	} else {
		/*
		 * Entry already present.  Assert that the caller doesn't want
		 * a hugepage at this level, and that there isn't a hugepage at
		 * this level.
		 */
		TEST_ASSERT(current_level != target_level,
			    "Cannot create hugepage at level: %u, vaddr: 0x%lx\n",
			    current_level, vaddr);
		TEST_ASSERT(!(*pte & PTE_LARGE_MASK),
			    "Cannot create page table at level: %u, vaddr: 0x%lx\n",
			    current_level, vaddr);
	}
	return pte;
}

void __virt_pg_map(struct kvm_vm *vm, uint64_t vaddr, uint64_t paddr, int level)
{
	const uint64_t pg_size = PG_LEVEL_SIZE(level);
	uint64_t *pml4e, *pdpe, *pde;
	uint64_t *pte;

	TEST_ASSERT(vm->mode == VM_MODE_PXXV48_4K,
		    "Unknown or unsupported guest mode, mode: 0x%x", vm->mode);

	TEST_ASSERT((vaddr % pg_size) == 0,
		    "Virtual address not aligned,\n"
		    "vaddr: 0x%lx page size: 0x%lx", vaddr, pg_size);
	TEST_ASSERT(sparsebit_is_set(vm->vpages_valid, (vaddr >> vm->page_shift)),
		    "Invalid virtual address, vaddr: 0x%lx", vaddr);
	TEST_ASSERT((paddr % pg_size) == 0,
		    "Physical address not aligned,\n"
		    "  paddr: 0x%lx page size: 0x%lx", paddr, pg_size);
	TEST_ASSERT((paddr >> vm->page_shift) <= vm->max_gfn,
		    "Physical address beyond maximum supported,\n"
		    "  paddr: 0x%lx vm->max_gfn: 0x%lx vm->page_size: 0x%x",
		    paddr, vm->max_gfn, vm->page_size);

	/*
	 * Allocate upper level page tables, if not already present.  Return
	 * early if a hugepage was created.
	 */
	pml4e = virt_create_upper_pte(vm, vm->pgd >> vm->page_shift,
				      vaddr, paddr, PG_LEVEL_512G, level);
	if (*pml4e & PTE_LARGE_MASK)
		return;

	pdpe = virt_create_upper_pte(vm, PTE_GET_PFN(*pml4e), vaddr, paddr, PG_LEVEL_1G, level);
	if (*pdpe & PTE_LARGE_MASK)
		return;

	pde = virt_create_upper_pte(vm, PTE_GET_PFN(*pdpe), vaddr, paddr, PG_LEVEL_2M, level);
	if (*pde & PTE_LARGE_MASK)
		return;

	/* Fill in page table entry. */
	pte = virt_get_pte(vm, PTE_GET_PFN(*pde), vaddr, PG_LEVEL_4K);
	TEST_ASSERT(!(*pte & PTE_PRESENT_MASK),
		    "PTE already present for 4k page at vaddr: 0x%lx\n", vaddr);
	*pte = PTE_PRESENT_MASK | PTE_WRITABLE_MASK | (paddr & PHYSICAL_PAGE_MASK);
}

void virt_arch_pg_map(struct kvm_vm *vm, uint64_t vaddr, uint64_t paddr)
{
	__virt_pg_map(vm, vaddr, paddr, PG_LEVEL_4K);
}

static uint64_t *_vm_get_page_table_entry(struct kvm_vm *vm,
					  struct kvm_vcpu *vcpu,
					  uint64_t vaddr)
{
	uint16_t index[4];
	uint64_t *pml4e, *pdpe, *pde;
	uint64_t *pte;
	struct kvm_cpuid_entry2 *entry;
	struct kvm_sregs sregs;
	int max_phy_addr;
	uint64_t rsvd_mask = 0;

	entry = kvm_get_supported_cpuid_index(0x80000008, 0);
	max_phy_addr = entry->eax & 0x000000ff;
	/* Set the high bits in the reserved mask. */
	if (max_phy_addr < 52)
		rsvd_mask = GENMASK_ULL(51, max_phy_addr);

	/*
	 * SDM vol 3, fig 4-11 "Formats of CR3 and Paging-Structure Entries
	 * with 4-Level Paging and 5-Level Paging".
	 * If IA32_EFER.NXE = 0 and the P flag of a paging-structure entry is 1,
	 * the XD flag (bit 63) is reserved.
	 */
	vcpu_sregs_get(vcpu, &sregs);
	if ((sregs.efer & EFER_NX) == 0) {
		rsvd_mask |= PTE_NX_MASK;
	}

	TEST_ASSERT(vm->mode == VM_MODE_PXXV48_4K, "Attempt to use "
		"unknown or unsupported guest mode, mode: 0x%x", vm->mode);
	TEST_ASSERT(sparsebit_is_set(vm->vpages_valid,
		(vaddr >> vm->page_shift)),
		"Invalid virtual address, vaddr: 0x%lx",
		vaddr);
	/*
	 * Based on the mode check above there are 48 bits in the vaddr, so
	 * shift 16 to sign extend the last bit (bit-47),
	 */
	TEST_ASSERT(vaddr == (((int64_t)vaddr << 16) >> 16),
		"Canonical check failed.  The virtual address is invalid.");

	index[0] = (vaddr >> 12) & 0x1ffu;
	index[1] = (vaddr >> 21) & 0x1ffu;
	index[2] = (vaddr >> 30) & 0x1ffu;
	index[3] = (vaddr >> 39) & 0x1ffu;

	pml4e = addr_gpa2hva(vm, vm->pgd);
	TEST_ASSERT(pml4e[index[3]] & PTE_PRESENT_MASK,
		"Expected pml4e to be present for gva: 0x%08lx", vaddr);
	TEST_ASSERT((pml4e[index[3]] & (rsvd_mask | PTE_LARGE_MASK)) == 0,
		"Unexpected reserved bits set.");

	pdpe = addr_gpa2hva(vm, PTE_GET_PFN(pml4e[index[3]]) * vm->page_size);
	TEST_ASSERT(pdpe[index[2]] & PTE_PRESENT_MASK,
		"Expected pdpe to be present for gva: 0x%08lx", vaddr);
	TEST_ASSERT(!(pdpe[index[2]] & PTE_LARGE_MASK),
		"Expected pdpe to map a pde not a 1-GByte page.");
	TEST_ASSERT((pdpe[index[2]] & rsvd_mask) == 0,
		"Unexpected reserved bits set.");

	pde = addr_gpa2hva(vm, PTE_GET_PFN(pdpe[index[2]]) * vm->page_size);
	TEST_ASSERT(pde[index[1]] & PTE_PRESENT_MASK,
		"Expected pde to be present for gva: 0x%08lx", vaddr);
	TEST_ASSERT(!(pde[index[1]] & PTE_LARGE_MASK),
		"Expected pde to map a pte not a 2-MByte page.");
	TEST_ASSERT((pde[index[1]] & rsvd_mask) == 0,
		"Unexpected reserved bits set.");

	pte = addr_gpa2hva(vm, PTE_GET_PFN(pde[index[1]]) * vm->page_size);
	TEST_ASSERT(pte[index[0]] & PTE_PRESENT_MASK,
		"Expected pte to be present for gva: 0x%08lx", vaddr);

	return &pte[index[0]];
}

uint64_t vm_get_page_table_entry(struct kvm_vm *vm, struct kvm_vcpu *vcpu,
				 uint64_t vaddr)
{
	uint64_t *pte = _vm_get_page_table_entry(vm, vcpu, vaddr);

	return *(uint64_t *)pte;
}

void vm_set_page_table_entry(struct kvm_vm *vm, struct kvm_vcpu *vcpu,
			     uint64_t vaddr, uint64_t pte)
{
	uint64_t *new_pte = _vm_get_page_table_entry(vm, vcpu, vaddr);

	*(uint64_t *)new_pte = pte;
}

void virt_arch_dump(FILE *stream, struct kvm_vm *vm, uint8_t indent)
{
	uint64_t *pml4e, *pml4e_start;
	uint64_t *pdpe, *pdpe_start;
	uint64_t *pde, *pde_start;
	uint64_t *pte, *pte_start;

	if (!vm->pgd_created)
		return;

	fprintf(stream, "%*s                                          "
		"                no\n", indent, "");
	fprintf(stream, "%*s      index hvaddr         gpaddr         "
		"addr         w exec dirty\n",
		indent, "");
	pml4e_start = (uint64_t *) addr_gpa2hva(vm, vm->pgd);
	for (uint16_t n1 = 0; n1 <= 0x1ffu; n1++) {
		pml4e = &pml4e_start[n1];
		if (!(*pml4e & PTE_PRESENT_MASK))
			continue;
		fprintf(stream, "%*spml4e 0x%-3zx %p 0x%-12lx 0x%-10llx %u "
			" %u\n",
			indent, "",
			pml4e - pml4e_start, pml4e,
			addr_hva2gpa(vm, pml4e), PTE_GET_PFN(*pml4e),
			!!(*pml4e & PTE_WRITABLE_MASK), !!(*pml4e & PTE_NX_MASK));

		pdpe_start = addr_gpa2hva(vm, *pml4e & PHYSICAL_PAGE_MASK);
		for (uint16_t n2 = 0; n2 <= 0x1ffu; n2++) {
			pdpe = &pdpe_start[n2];
			if (!(*pdpe & PTE_PRESENT_MASK))
				continue;
			fprintf(stream, "%*spdpe  0x%-3zx %p 0x%-12lx 0x%-10llx "
				"%u  %u\n",
				indent, "",
				pdpe - pdpe_start, pdpe,
				addr_hva2gpa(vm, pdpe),
				PTE_GET_PFN(*pdpe), !!(*pdpe & PTE_WRITABLE_MASK),
				!!(*pdpe & PTE_NX_MASK));

			pde_start = addr_gpa2hva(vm, *pdpe & PHYSICAL_PAGE_MASK);
			for (uint16_t n3 = 0; n3 <= 0x1ffu; n3++) {
				pde = &pde_start[n3];
				if (!(*pde & PTE_PRESENT_MASK))
					continue;
				fprintf(stream, "%*spde   0x%-3zx %p "
					"0x%-12lx 0x%-10llx %u  %u\n",
					indent, "", pde - pde_start, pde,
					addr_hva2gpa(vm, pde),
					PTE_GET_PFN(*pde), !!(*pde & PTE_WRITABLE_MASK),
					!!(*pde & PTE_NX_MASK));

				pte_start = addr_gpa2hva(vm, *pde & PHYSICAL_PAGE_MASK);
				for (uint16_t n4 = 0; n4 <= 0x1ffu; n4++) {
					pte = &pte_start[n4];
					if (!(*pte & PTE_PRESENT_MASK))
						continue;
					fprintf(stream, "%*spte   0x%-3zx %p "
						"0x%-12lx 0x%-10llx %u  %u "
						"    %u    0x%-10lx\n",
						indent, "",
						pte - pte_start, pte,
						addr_hva2gpa(vm, pte),
						PTE_GET_PFN(*pte),
						!!(*pte & PTE_WRITABLE_MASK),
						!!(*pte & PTE_NX_MASK),
						!!(*pte & PTE_DIRTY_MASK),
						((uint64_t) n1 << 27)
							| ((uint64_t) n2 << 18)
							| ((uint64_t) n3 << 9)
							| ((uint64_t) n4));
				}
			}
		}
	}
}

/*
 * Set Unusable Segment
 *
 * Input Args: None
 *
 * Output Args:
 *   segp - Pointer to segment register
 *
 * Return: None
 *
 * Sets the segment register pointed to by @segp to an unusable state.
 */
static void kvm_seg_set_unusable(struct kvm_segment *segp)
{
	memset(segp, 0, sizeof(*segp));
	segp->unusable = true;
}

static void kvm_seg_fill_gdt_64bit(struct kvm_vm *vm, struct kvm_segment *segp)
{
	void *gdt = addr_gva2hva(vm, vm->gdt);
	struct desc64 *desc = gdt + (segp->selector >> 3) * 8;

	desc->limit0 = segp->limit & 0xFFFF;
	desc->base0 = segp->base & 0xFFFF;
	desc->base1 = segp->base >> 16;
	desc->type = segp->type;
	desc->s = segp->s;
	desc->dpl = segp->dpl;
	desc->p = segp->present;
	desc->limit1 = segp->limit >> 16;
	desc->avl = segp->avl;
	desc->l = segp->l;
	desc->db = segp->db;
	desc->g = segp->g;
	desc->base2 = segp->base >> 24;
	if (!segp->s)
		desc->base3 = segp->base >> 32;
}


/*
 * Set Long Mode Flat Kernel Code Segment
 *
 * Input Args:
 *   vm - VM whose GDT is being filled, or NULL to only write segp
 *   selector - selector value
 *
 * Output Args:
 *   segp - Pointer to KVM segment
 *
 * Return: None
 *
 * Sets up the KVM segment pointed to by @segp, to be a code segment
 * with the selector value given by @selector.
 */
static void kvm_seg_set_kernel_code_64bit(struct kvm_vm *vm, uint16_t selector,
	struct kvm_segment *segp)
{
	memset(segp, 0, sizeof(*segp));
	segp->selector = selector;
	segp->limit = 0xFFFFFFFFu;
	segp->s = 0x1; /* kTypeCodeData */
	segp->type = 0x08 | 0x01 | 0x02; /* kFlagCode | kFlagCodeAccessed
					  * | kFlagCodeReadable
					  */
	segp->g = true;
	segp->l = true;
	segp->present = 1;
	if (vm)
		kvm_seg_fill_gdt_64bit(vm, segp);
}

/*
 * Set Long Mode Flat Kernel Data Segment
 *
 * Input Args:
 *   vm - VM whose GDT is being filled, or NULL to only write segp
 *   selector - selector value
 *
 * Output Args:
 *   segp - Pointer to KVM segment
 *
 * Return: None
 *
 * Sets up the KVM segment pointed to by @segp, to be a data segment
 * with the selector value given by @selector.
 */
static void kvm_seg_set_kernel_data_64bit(struct kvm_vm *vm, uint16_t selector,
	struct kvm_segment *segp)
{
	memset(segp, 0, sizeof(*segp));
	segp->selector = selector;
	segp->limit = 0xFFFFFFFFu;
	segp->s = 0x1; /* kTypeCodeData */
	segp->type = 0x00 | 0x01 | 0x02; /* kFlagData | kFlagDataAccessed
					  * | kFlagDataWritable
					  */
	segp->g = true;
	segp->present = true;
	if (vm)
		kvm_seg_fill_gdt_64bit(vm, segp);
}

vm_paddr_t addr_arch_gva2gpa(struct kvm_vm *vm, vm_vaddr_t gva)
{
	uint16_t index[4];
	uint64_t *pml4e, *pdpe, *pde;
	uint64_t *pte;

	TEST_ASSERT(vm->mode == VM_MODE_PXXV48_4K, "Attempt to use "
		"unknown or unsupported guest mode, mode: 0x%x", vm->mode);

	index[0] = (gva >> 12) & 0x1ffu;
	index[1] = (gva >> 21) & 0x1ffu;
	index[2] = (gva >> 30) & 0x1ffu;
	index[3] = (gva >> 39) & 0x1ffu;

	if (!vm->pgd_created)
		goto unmapped_gva;
	pml4e = addr_gpa2hva(vm, vm->pgd);
	if (!(pml4e[index[3]] & PTE_PRESENT_MASK))
		goto unmapped_gva;

	pdpe = addr_gpa2hva(vm, PTE_GET_PFN(pml4e[index[3]]) * vm->page_size);
	if (!(pdpe[index[2]] & PTE_PRESENT_MASK))
		goto unmapped_gva;

	pde = addr_gpa2hva(vm, PTE_GET_PFN(pdpe[index[2]]) * vm->page_size);
	if (!(pde[index[1]] & PTE_PRESENT_MASK))
		goto unmapped_gva;

	pte = addr_gpa2hva(vm, PTE_GET_PFN(pde[index[1]]) * vm->page_size);
	if (!(pte[index[0]] & PTE_PRESENT_MASK))
		goto unmapped_gva;

	return (PTE_GET_PFN(pte[index[0]]) * vm->page_size) + (gva & ~PAGE_MASK);

unmapped_gva:
	TEST_FAIL("No mapping for vm virtual address, gva: 0x%lx", gva);
	exit(EXIT_FAILURE);
}

static void kvm_setup_gdt(struct kvm_vm *vm, struct kvm_dtable *dt)
{
	if (!vm->gdt)
		vm->gdt = vm_vaddr_alloc_page(vm);

	dt->base = vm->gdt;
	dt->limit = getpagesize();
}

static void kvm_setup_tss_64bit(struct kvm_vm *vm, struct kvm_segment *segp,
				int selector)
{
	if (!vm->tss)
		vm->tss = vm_vaddr_alloc_page(vm);

	memset(segp, 0, sizeof(*segp));
	segp->base = vm->tss;
	segp->limit = 0x67;
	segp->selector = selector;
	segp->type = 0xb;
	segp->present = 1;
	kvm_seg_fill_gdt_64bit(vm, segp);
}

static void vcpu_setup(struct kvm_vm *vm, struct kvm_vcpu *vcpu)
{
	struct kvm_sregs sregs;

	/* Set mode specific system register values. */
	vcpu_sregs_get(vcpu, &sregs);

	sregs.idt.limit = 0;

	kvm_setup_gdt(vm, &sregs.gdt);

	switch (vm->mode) {
	case VM_MODE_PXXV48_4K:
		sregs.cr0 = X86_CR0_PE | X86_CR0_NE | X86_CR0_PG;
		sregs.cr4 |= X86_CR4_PAE | X86_CR4_OSFXSR;
		sregs.efer |= (EFER_LME | EFER_LMA | EFER_NX);

		kvm_seg_set_unusable(&sregs.ldt);
		kvm_seg_set_kernel_code_64bit(vm, DEFAULT_CODE_SELECTOR, &sregs.cs);
		kvm_seg_set_kernel_data_64bit(vm, DEFAULT_DATA_SELECTOR, &sregs.ds);
		kvm_seg_set_kernel_data_64bit(vm, DEFAULT_DATA_SELECTOR, &sregs.es);
		kvm_setup_tss_64bit(vm, &sregs.tr, 0x18);
		break;

	default:
		TEST_FAIL("Unknown guest mode, mode: 0x%x", vm->mode);
	}

	sregs.cr3 = vm->pgd;
	vcpu_sregs_set(vcpu, &sregs);
}

#define CPUID_XFD_BIT (1 << 4)
static bool is_xfd_supported(void)
{
	int eax, ebx, ecx, edx;
	const int leaf = 0xd, subleaf = 0x1;

	__asm__ __volatile__(
		"cpuid"
		: /* output */ "=a"(eax), "=b"(ebx),
		  "=c"(ecx), "=d"(edx)
		: /* input */ "0"(leaf), "2"(subleaf));

	return !!(eax & CPUID_XFD_BIT);
}

void vm_xsave_req_perm(int bit)
{
	int kvm_fd;
	u64 bitmask;
	long rc;
	struct kvm_device_attr attr = {
		.group = 0,
		.attr = KVM_X86_XCOMP_GUEST_SUPP,
		.addr = (unsigned long) &bitmask
	};

	kvm_fd = open_kvm_dev_path_or_exit();
	rc = __kvm_ioctl(kvm_fd, KVM_GET_DEVICE_ATTR, &attr);
	close(kvm_fd);

	if (rc == -1 && (errno == ENXIO || errno == EINVAL))
		exit(KSFT_SKIP);
	TEST_ASSERT(rc == 0, "KVM_GET_DEVICE_ATTR(0, KVM_X86_XCOMP_GUEST_SUPP) error: %ld", rc);

	TEST_REQUIRE(bitmask & (1ULL << bit));

	TEST_REQUIRE(is_xfd_supported());

	rc = syscall(SYS_arch_prctl, ARCH_REQ_XCOMP_GUEST_PERM, bit);

	/*
	 * The older kernel version(<5.15) can't support
	 * ARCH_REQ_XCOMP_GUEST_PERM and directly return.
	 */
	if (rc)
		return;

	rc = syscall(SYS_arch_prctl, ARCH_GET_XCOMP_GUEST_PERM, &bitmask);
	TEST_ASSERT(rc == 0, "prctl(ARCH_GET_XCOMP_GUEST_PERM) error: %ld", rc);
	TEST_ASSERT(bitmask & (1ULL << bit),
		    "prctl(ARCH_REQ_XCOMP_GUEST_PERM) failure bitmask=0x%lx",
		    bitmask);
}

struct kvm_vcpu *vm_arch_vcpu_add(struct kvm_vm *vm, uint32_t vcpu_id,
				  void *guest_code)
{
	struct kvm_mp_state mp_state;
	struct kvm_regs regs;
	vm_vaddr_t stack_vaddr;
	struct kvm_vcpu *vcpu;

	stack_vaddr = vm_vaddr_alloc(vm, DEFAULT_STACK_PGS * getpagesize(),
				     DEFAULT_GUEST_STACK_VADDR_MIN);

	vcpu = __vm_vcpu_add(vm, vcpu_id);
	vcpu_set_cpuid(vcpu, kvm_get_supported_cpuid());
	vcpu_setup(vm, vcpu);

	/* Setup guest general purpose registers */
	vcpu_regs_get(vcpu, &regs);
	regs.rflags = regs.rflags | 0x2;
	regs.rsp = stack_vaddr + (DEFAULT_STACK_PGS * getpagesize());
	regs.rip = (unsigned long) guest_code;
	vcpu_regs_set(vcpu, &regs);

	/* Setup the MP state */
	mp_state.mp_state = 0;
	vcpu_mp_state_set(vcpu, &mp_state);

	return vcpu;
}

/*
 * Allocate an instance of struct kvm_cpuid2
 *
 * Input Args: None
 *
 * Output Args: None
 *
 * Return: A pointer to the allocated struct. The caller is responsible
 * for freeing this struct.
 *
 * Since kvm_cpuid2 uses a 0-length array to allow a the size of the
 * array to be decided at allocation time, allocation is slightly
 * complicated. This function uses a reasonable default length for
 * the array and performs the appropriate allocation.
 */
static struct kvm_cpuid2 *allocate_kvm_cpuid2(void)
{
	struct kvm_cpuid2 *cpuid;
	int nent = 100;
	size_t size;

	size = sizeof(*cpuid);
	size += nent * sizeof(struct kvm_cpuid_entry2);
	cpuid = malloc(size);
	if (!cpuid) {
		perror("malloc");
		abort();
	}

	cpuid->nent = nent;

	return cpuid;
}

/*
 * KVM Supported CPUID Get
 *
 * Input Args: None
 *
 * Output Args:
 *
 * Return: The supported KVM CPUID
 *
 * Get the guest CPUID supported by KVM.
 */
struct kvm_cpuid2 *kvm_get_supported_cpuid(void)
{
	static struct kvm_cpuid2 *cpuid;
	int kvm_fd;

	if (cpuid)
		return cpuid;

	cpuid = allocate_kvm_cpuid2();
	kvm_fd = open_kvm_dev_path_or_exit();

	kvm_ioctl(kvm_fd, KVM_GET_SUPPORTED_CPUID, cpuid);

	close(kvm_fd);
	return cpuid;
}

uint64_t kvm_get_feature_msr(uint64_t msr_index)
{
	struct {
		struct kvm_msrs header;
		struct kvm_msr_entry entry;
	} buffer = {};
	int r, kvm_fd;

	buffer.header.nmsrs = 1;
	buffer.entry.index = msr_index;
	kvm_fd = open_kvm_dev_path_or_exit();

	r = __kvm_ioctl(kvm_fd, KVM_GET_MSRS, &buffer.header);
	TEST_ASSERT(r == 1, KVM_IOCTL_ERROR(KVM_GET_MSRS, r));

	close(kvm_fd);
	return buffer.entry.data;
}

struct kvm_cpuid2 *vcpu_get_cpuid(struct kvm_vcpu *vcpu)
{
	struct kvm_cpuid2 *cpuid;
	int max_ent;
	int rc = -1;

	cpuid = allocate_kvm_cpuid2();
	max_ent = cpuid->nent;

	for (cpuid->nent = 1; cpuid->nent <= max_ent; cpuid->nent++) {
		rc = __vcpu_ioctl(vcpu, KVM_GET_CPUID2, cpuid);
		if (!rc)
			break;

		TEST_ASSERT(rc == -1 && errno == E2BIG,
			    "KVM_GET_CPUID2 should either succeed or give E2BIG: %d %d",
			    rc, errno);
	}

	TEST_ASSERT(!rc, KVM_IOCTL_ERROR(KVM_GET_CPUID2, rc));
	return cpuid;
}



/*
 * Locate a cpuid entry.
 *
 * Input Args:
 *   function: The function of the cpuid entry to find.
 *   index: The index of the cpuid entry.
 *
 * Output Args: None
 *
 * Return: A pointer to the cpuid entry. Never returns NULL.
 */
struct kvm_cpuid_entry2 *
kvm_get_supported_cpuid_index(uint32_t function, uint32_t index)
{
	struct kvm_cpuid2 *cpuid;
	struct kvm_cpuid_entry2 *entry = NULL;
	int i;

	cpuid = kvm_get_supported_cpuid();
	for (i = 0; i < cpuid->nent; i++) {
		if (cpuid->entries[i].function == function &&
		    cpuid->entries[i].index == index) {
			entry = &cpuid->entries[i];
			break;
		}
	}

	TEST_ASSERT(entry, "Guest CPUID entry not found: (EAX=%x, ECX=%x).",
		    function, index);
	return entry;
}

uint64_t vcpu_get_msr(struct kvm_vcpu *vcpu, uint64_t msr_index)
{
	struct {
		struct kvm_msrs header;
		struct kvm_msr_entry entry;
	} buffer = {};

	buffer.header.nmsrs = 1;
	buffer.entry.index = msr_index;

	vcpu_msrs_get(vcpu, &buffer.header);

	return buffer.entry.data;
}

int _vcpu_set_msr(struct kvm_vcpu *vcpu, uint64_t msr_index, uint64_t msr_value)
{
	struct {
		struct kvm_msrs header;
		struct kvm_msr_entry entry;
	} buffer = {};

	memset(&buffer, 0, sizeof(buffer));
	buffer.header.nmsrs = 1;
	buffer.entry.index = msr_index;
	buffer.entry.data = msr_value;

	return __vcpu_ioctl(vcpu, KVM_SET_MSRS, &buffer.header);
}

void vcpu_args_set(struct kvm_vcpu *vcpu, unsigned int num, ...)
{
	va_list ap;
	struct kvm_regs regs;

	TEST_ASSERT(num >= 1 && num <= 6, "Unsupported number of args,\n"
		    "  num: %u\n",
		    num);

	va_start(ap, num);
	vcpu_regs_get(vcpu, &regs);

	if (num >= 1)
		regs.rdi = va_arg(ap, uint64_t);

	if (num >= 2)
		regs.rsi = va_arg(ap, uint64_t);

	if (num >= 3)
		regs.rdx = va_arg(ap, uint64_t);

	if (num >= 4)
		regs.rcx = va_arg(ap, uint64_t);

	if (num >= 5)
		regs.r8 = va_arg(ap, uint64_t);

	if (num >= 6)
		regs.r9 = va_arg(ap, uint64_t);

	vcpu_regs_set(vcpu, &regs);
	va_end(ap);
}

void vcpu_arch_dump(FILE *stream, struct kvm_vcpu *vcpu, uint8_t indent)
{
	struct kvm_regs regs;
	struct kvm_sregs sregs;

	fprintf(stream, "%*svCPU ID: %u\n", indent, "", vcpu->id);

	fprintf(stream, "%*sregs:\n", indent + 2, "");
	vcpu_regs_get(vcpu, &regs);
	regs_dump(stream, &regs, indent + 4);

	fprintf(stream, "%*ssregs:\n", indent + 2, "");
	vcpu_sregs_get(vcpu, &sregs);
	sregs_dump(stream, &sregs, indent + 4);
}

static struct kvm_msr_list *__kvm_get_msr_index_list(bool feature_msrs)
{
	struct kvm_msr_list *list;
	struct kvm_msr_list nmsrs;
	int kvm_fd, r;

	kvm_fd = open_kvm_dev_path_or_exit();

	nmsrs.nmsrs = 0;
	if (!feature_msrs)
		r = __kvm_ioctl(kvm_fd, KVM_GET_MSR_INDEX_LIST, &nmsrs);
	else
		r = __kvm_ioctl(kvm_fd, KVM_GET_MSR_FEATURE_INDEX_LIST, &nmsrs);

	TEST_ASSERT(r == -1 && errno == E2BIG,
		    "Expected -E2BIG, got rc: %i errno: %i (%s)",
		    r, errno, strerror(errno));

	list = malloc(sizeof(*list) + nmsrs.nmsrs * sizeof(list->indices[0]));
	TEST_ASSERT(list, "-ENOMEM when allocating MSR index list");
	list->nmsrs = nmsrs.nmsrs;

	if (!feature_msrs)
		kvm_ioctl(kvm_fd, KVM_GET_MSR_INDEX_LIST, list);
	else
		kvm_ioctl(kvm_fd, KVM_GET_MSR_FEATURE_INDEX_LIST, list);
	close(kvm_fd);

	TEST_ASSERT(list->nmsrs == nmsrs.nmsrs,
		    "Number of MSRs in list changed, was %d, now %d",
		    nmsrs.nmsrs, list->nmsrs);
	return list;
}

const struct kvm_msr_list *kvm_get_msr_index_list(void)
{
	static const struct kvm_msr_list *list;

	if (!list)
		list = __kvm_get_msr_index_list(false);
	return list;
}


const struct kvm_msr_list *kvm_get_feature_msr_index_list(void)
{
	static const struct kvm_msr_list *list;

	if (!list)
		list = __kvm_get_msr_index_list(true);
	return list;
}

bool kvm_msr_is_in_save_restore_list(uint32_t msr_index)
{
	const struct kvm_msr_list *list = kvm_get_msr_index_list();
	int i;

	for (i = 0; i < list->nmsrs; ++i) {
		if (list->indices[i] == msr_index)
			return true;
	}

	return false;
}

static void vcpu_save_xsave_state(struct kvm_vcpu *vcpu,
				  struct kvm_x86_state *state)
{
	int size = vm_check_cap(vcpu->vm, KVM_CAP_XSAVE2);

	if (size) {
		state->xsave = malloc(size);
		vcpu_xsave2_get(vcpu, state->xsave);
	} else {
		state->xsave = malloc(sizeof(struct kvm_xsave));
		vcpu_xsave_get(vcpu, state->xsave);
	}
}

struct kvm_x86_state *vcpu_save_state(struct kvm_vcpu *vcpu)
{
	const struct kvm_msr_list *msr_list = kvm_get_msr_index_list();
	struct kvm_x86_state *state;
	int i;

	static int nested_size = -1;

	if (nested_size == -1) {
		nested_size = kvm_check_cap(KVM_CAP_NESTED_STATE);
		TEST_ASSERT(nested_size <= sizeof(state->nested_),
			    "Nested state size too big, %i > %zi",
			    nested_size, sizeof(state->nested_));
	}

	/*
	 * When KVM exits to userspace with KVM_EXIT_IO, KVM guarantees
	 * guest state is consistent only after userspace re-enters the
	 * kernel with KVM_RUN.  Complete IO prior to migrating state
	 * to a new VM.
	 */
	vcpu_run_complete_io(vcpu);

	state = malloc(sizeof(*state) + msr_list->nmsrs * sizeof(state->msrs.entries[0]));

	vcpu_events_get(vcpu, &state->events);
	vcpu_mp_state_get(vcpu, &state->mp_state);
	vcpu_regs_get(vcpu, &state->regs);
	vcpu_save_xsave_state(vcpu, state);

	if (kvm_has_cap(KVM_CAP_XCRS))
		vcpu_xcrs_get(vcpu, &state->xcrs);

	vcpu_sregs_get(vcpu, &state->sregs);

	if (nested_size) {
		state->nested.size = sizeof(state->nested_);

		vcpu_nested_state_get(vcpu, &state->nested);
		TEST_ASSERT(state->nested.size <= nested_size,
			    "Nested state size too big, %i (KVM_CHECK_CAP gave %i)",
			    state->nested.size, nested_size);
	} else {
		state->nested.size = 0;
	}

	state->msrs.nmsrs = msr_list->nmsrs;
	for (i = 0; i < msr_list->nmsrs; i++)
		state->msrs.entries[i].index = msr_list->indices[i];
	vcpu_msrs_get(vcpu, &state->msrs);

	vcpu_debugregs_get(vcpu, &state->debugregs);

	return state;
}

void vcpu_load_state(struct kvm_vcpu *vcpu, struct kvm_x86_state *state)
{
	vcpu_sregs_set(vcpu, &state->sregs);
	vcpu_msrs_set(vcpu, &state->msrs);

	if (kvm_has_cap(KVM_CAP_XCRS))
		vcpu_xcrs_set(vcpu, &state->xcrs);

	vcpu_xsave_set(vcpu,  state->xsave);
	vcpu_events_set(vcpu, &state->events);
	vcpu_mp_state_set(vcpu, &state->mp_state);
	vcpu_debugregs_set(vcpu, &state->debugregs);
	vcpu_regs_set(vcpu, &state->regs);

	if (state->nested.size)
		vcpu_nested_state_set(vcpu, &state->nested);
}

void kvm_x86_state_cleanup(struct kvm_x86_state *state)
{
	free(state->xsave);
	free(state);
}

static bool cpu_vendor_string_is(const char *vendor)
{
	const uint32_t *chunk = (const uint32_t *)vendor;
	int eax, ebx, ecx, edx;
	const int leaf = 0;

	__asm__ __volatile__(
		"cpuid"
		: /* output */ "=a"(eax), "=b"(ebx),
		  "=c"(ecx), "=d"(edx)
		: /* input */ "0"(leaf), "2"(0));

	return (ebx == chunk[0] && edx == chunk[1] && ecx == chunk[2]);
}

bool is_intel_cpu(void)
{
	return cpu_vendor_string_is("GenuineIntel");
}

/*
 * Exclude early K5 samples with a vendor string of "AMDisbetter!"
 */
bool is_amd_cpu(void)
{
	return cpu_vendor_string_is("AuthenticAMD");
}

uint32_t kvm_get_cpuid_max_basic(void)
{
	return kvm_get_supported_cpuid_entry(0)->eax;
}

uint32_t kvm_get_cpuid_max_extended(void)
{
	return kvm_get_supported_cpuid_entry(0x80000000)->eax;
}

void kvm_get_cpu_address_width(unsigned int *pa_bits, unsigned int *va_bits)
{
	struct kvm_cpuid_entry2 *entry;
	bool pae;

	/* SDM 4.1.4 */
	if (kvm_get_cpuid_max_extended() < 0x80000008) {
		pae = kvm_get_supported_cpuid_entry(1)->edx & (1 << 6);
		*pa_bits = pae ? 36 : 32;
		*va_bits = 32;
	} else {
		entry = kvm_get_supported_cpuid_entry(0x80000008);
		*pa_bits = entry->eax & 0xff;
		*va_bits = (entry->eax >> 8) & 0xff;
	}
}

struct idt_entry {
	uint16_t offset0;
	uint16_t selector;
	uint16_t ist : 3;
	uint16_t : 5;
	uint16_t type : 4;
	uint16_t : 1;
	uint16_t dpl : 2;
	uint16_t p : 1;
	uint16_t offset1;
	uint32_t offset2; uint32_t reserved;
};

static void set_idt_entry(struct kvm_vm *vm, int vector, unsigned long addr,
			  int dpl, unsigned short selector)
{
	struct idt_entry *base =
		(struct idt_entry *)addr_gva2hva(vm, vm->idt);
	struct idt_entry *e = &base[vector];

	memset(e, 0, sizeof(*e));
	e->offset0 = addr;
	e->selector = selector;
	e->ist = 0;
	e->type = 14;
	e->dpl = dpl;
	e->p = 1;
	e->offset1 = addr >> 16;
	e->offset2 = addr >> 32;
}

void kvm_exit_unexpected_vector(uint32_t value)
{
	ucall(UCALL_UNHANDLED, 1, value);
}

void route_exception(struct ex_regs *regs)
{
	typedef void(*handler)(struct ex_regs *);
	handler *handlers = (handler *)exception_handlers;

	if (handlers && handlers[regs->vector]) {
		handlers[regs->vector](regs);
		return;
	}

	kvm_exit_unexpected_vector(regs->vector);
}

void vm_init_descriptor_tables(struct kvm_vm *vm)
{
	extern void *idt_handlers;
	int i;

	vm->idt = vm_vaddr_alloc_page(vm);
	vm->handlers = vm_vaddr_alloc_page(vm);
	/* Handlers have the same address in both address spaces.*/
	for (i = 0; i < NUM_INTERRUPTS; i++)
		set_idt_entry(vm, i, (unsigned long)(&idt_handlers)[i], 0,
			DEFAULT_CODE_SELECTOR);
}

void vcpu_init_descriptor_tables(struct kvm_vcpu *vcpu)
{
	struct kvm_vm *vm = vcpu->vm;
	struct kvm_sregs sregs;

	vcpu_sregs_get(vcpu, &sregs);
	sregs.idt.base = vm->idt;
	sregs.idt.limit = NUM_INTERRUPTS * sizeof(struct idt_entry) - 1;
	sregs.gdt.base = vm->gdt;
	sregs.gdt.limit = getpagesize() - 1;
	kvm_seg_set_kernel_data_64bit(NULL, DEFAULT_DATA_SELECTOR, &sregs.gs);
	vcpu_sregs_set(vcpu, &sregs);
	*(vm_vaddr_t *)addr_gva2hva(vm, (vm_vaddr_t)(&exception_handlers)) = vm->handlers;
}

void vm_install_exception_handler(struct kvm_vm *vm, int vector,
			       void (*handler)(struct ex_regs *))
{
	vm_vaddr_t *handlers = (vm_vaddr_t *)addr_gva2hva(vm, vm->handlers);

	handlers[vector] = (vm_vaddr_t)handler;
}

void assert_on_unhandled_exception(struct kvm_vcpu *vcpu)
{
	struct ucall uc;

	if (get_ucall(vcpu, &uc) == UCALL_UNHANDLED) {
		uint64_t vector = uc.args[0];

		TEST_FAIL("Unexpected vectored event in guest (vector:0x%lx)",
			  vector);
	}
}

struct kvm_cpuid_entry2 *get_cpuid(struct kvm_cpuid2 *cpuid, uint32_t function,
				   uint32_t index)
{
	int i;

	for (i = 0; i < cpuid->nent; i++) {
		struct kvm_cpuid_entry2 *cur = &cpuid->entries[i];

		if (cur->function == function && cur->index == index)
			return cur;
	}

	TEST_FAIL("CPUID function 0x%x index 0x%x not found ", function, index);

	return NULL;
}

bool set_cpuid(struct kvm_cpuid2 *cpuid,
	       struct kvm_cpuid_entry2 *ent)
{
	int i;

	for (i = 0; i < cpuid->nent; i++) {
		struct kvm_cpuid_entry2 *cur = &cpuid->entries[i];

		if (cur->function != ent->function || cur->index != ent->index)
			continue;

		memcpy(cur, ent, sizeof(struct kvm_cpuid_entry2));
		return true;
	}

	return false;
}

uint64_t kvm_hypercall(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
		       uint64_t a3)
{
	uint64_t r;

	asm volatile("vmcall"
		     : "=a"(r)
		     : "b"(a0), "c"(a1), "d"(a2), "S"(a3));
	return r;
}

struct kvm_cpuid2 *kvm_get_supported_hv_cpuid(void)
{
	static struct kvm_cpuid2 *cpuid;
	int kvm_fd;

	if (cpuid)
		return cpuid;

	cpuid = allocate_kvm_cpuid2();
	kvm_fd = open_kvm_dev_path_or_exit();

	kvm_ioctl(kvm_fd, KVM_GET_SUPPORTED_HV_CPUID, cpuid);

	close(kvm_fd);
	return cpuid;
}

void vcpu_set_hv_cpuid(struct kvm_vcpu *vcpu)
{
	static struct kvm_cpuid2 *cpuid_full;
	struct kvm_cpuid2 *cpuid_sys, *cpuid_hv;
	int i, nent = 0;

	if (!cpuid_full) {
		cpuid_sys = kvm_get_supported_cpuid();
		cpuid_hv = kvm_get_supported_hv_cpuid();

		cpuid_full = malloc(sizeof(*cpuid_full) +
				    (cpuid_sys->nent + cpuid_hv->nent) *
				    sizeof(struct kvm_cpuid_entry2));
		if (!cpuid_full) {
			perror("malloc");
			abort();
		}

		/* Need to skip KVM CPUID leaves 0x400000xx */
		for (i = 0; i < cpuid_sys->nent; i++) {
			if (cpuid_sys->entries[i].function >= 0x40000000 &&
			    cpuid_sys->entries[i].function < 0x40000100)
				continue;
			cpuid_full->entries[nent] = cpuid_sys->entries[i];
			nent++;
		}

		memcpy(&cpuid_full->entries[nent], cpuid_hv->entries,
		       cpuid_hv->nent * sizeof(struct kvm_cpuid_entry2));
		cpuid_full->nent = nent + cpuid_hv->nent;
	}

	vcpu_set_cpuid(vcpu, cpuid_full);
}

struct kvm_cpuid2 *vcpu_get_supported_hv_cpuid(struct kvm_vcpu *vcpu)
{
	static struct kvm_cpuid2 *cpuid;

	cpuid = allocate_kvm_cpuid2();

	vcpu_ioctl(vcpu, KVM_GET_SUPPORTED_HV_CPUID, cpuid);

	return cpuid;
}

unsigned long vm_compute_max_gfn(struct kvm_vm *vm)
{
	const unsigned long num_ht_pages = 12 << (30 - vm->page_shift); /* 12 GiB */
	unsigned long ht_gfn, max_gfn, max_pfn;
	uint32_t eax, ebx, ecx, edx, max_ext_leaf;

	max_gfn = (1ULL << (vm->pa_bits - vm->page_shift)) - 1;

	/* Avoid reserved HyperTransport region on AMD processors.  */
	if (!is_amd_cpu())
		return max_gfn;

	/* On parts with <40 physical address bits, the area is fully hidden */
	if (vm->pa_bits < 40)
		return max_gfn;

	/* Before family 17h, the HyperTransport area is just below 1T.  */
	ht_gfn = (1 << 28) - num_ht_pages;
	eax = 1;
	ecx = 0;
	cpuid(&eax, &ebx, &ecx, &edx);
	if (x86_family(eax) < 0x17)
		goto done;

	/*
	 * Otherwise it's at the top of the physical address space, possibly
	 * reduced due to SME by bits 11:6 of CPUID[0x8000001f].EBX.  Use
	 * the old conservative value if MAXPHYADDR is not enumerated.
	 */
	eax = 0x80000000;
	cpuid(&eax, &ebx, &ecx, &edx);
	max_ext_leaf = eax;
	if (max_ext_leaf < 0x80000008)
		goto done;

	eax = 0x80000008;
	cpuid(&eax, &ebx, &ecx, &edx);
	max_pfn = (1ULL << ((eax & 0xff) - vm->page_shift)) - 1;
	if (max_ext_leaf >= 0x8000001f) {
		eax = 0x8000001f;
		cpuid(&eax, &ebx, &ecx, &edx);
		max_pfn >>= (ebx >> 6) & 0x3f;
	}

	ht_gfn = max_pfn - num_ht_pages;
done:
	return min(max_gfn, ht_gfn - 1);
}

/* Returns true if kvm_intel was loaded with unrestricted_guest=1. */
bool vm_is_unrestricted_guest(struct kvm_vm *vm)
{
	char val = 'N';
	size_t count;
	FILE *f;

	/* Ensure that a KVM vendor-specific module is loaded. */
	if (vm == NULL)
		close(open_kvm_dev_path_or_exit());

	f = fopen("/sys/module/kvm_intel/parameters/unrestricted_guest", "r");
	if (f) {
		count = fread(&val, sizeof(char), 1, f);
		TEST_ASSERT(count == 1, "Unable to read from param file.");
		fclose(f);
	}

	return val == 'Y';
}
