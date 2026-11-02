/*	$NetBSD$	*/

/*
 * Kernel-only pmap for bring-up: enough for uvm to run in the translated
 * window at 0xf8000000. Kernel addresses below that window bypass translation,
 * so only this window has page tables. User pmaps come with M3.
 *
 * Table walks do not see the data cache, so tables live in the non-cached top
 * 16 MiB of RAM. Linux's PGD cache occupies the start of that region, and
 * devices it left running have no rings there.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/proc.h>

#include <uvm/uvm.h>
#include <uvm/uvm_physseg.h>

#define	ASI_MMU		0x10
#define	ASI_TLBPROBE	0x11
#define	MMU_PGT		0x24
#define	MMU_PGT_EN	0x1
#define	MMU_PGT_FL	0x2

#define	RAM_PA_END	0x7f000000UL	/* cached RAM */
#define	PGD_PA		PMAP_PGD_PA
#define	PT_PA		PMAP_PT_PA
#define	KVA_PDE_FIRST	(VM_MIN_KERNEL_ADDRESS >> PMAP_PGD_SHIFT)
#define	KVA_NPT		(PMAP_NPDE - KVA_PDE_FIRST)

/* The bits Linux uses for pmd_populate_kernel() and PAGE_KERNEL. */
#define	PDE_KERNEL	(PTE_U | PTE_W | PTE_D | PTE_R | PTE_X | PTE_V | PTE_C)
#define	PTE_KERNEL	(PTE_R | PTE_X | PTE_D | PTE_A | PTE_V | PTE_C)

static struct pmap kernel_pmap_store;
struct pmap * const kernel_pmap_ptr = &kernel_pmap_store;

static uint32_t *kva_pt;
static vaddr_t virtual_avail, virtual_end;

static inline uint32_t *
kvtopte(vaddr_t va)
{

	KASSERT(va >= VM_MIN_KERNEL_ADDRESS);
	return &kva_pt[(va - VM_MIN_KERNEL_ADDRESS) >> PAGE_SHIFT];
}

/*
 * A store to ASI_TLBPROBE writes the PTE of that address in the current tables
 * and drops the page from both TLBs. Measured with hw/probe/mmutest.c. Misses
 * are refilled by hardware, which also keeps A and D in the table.
 */
static inline void
pte_store(vaddr_t va, uint32_t pte)
{

	__asm volatile("sta %0, [%1] %2" : :
	    "r" (pte), "r" (va & ~PAGE_MASK), "n" (ASI_TLBPROBE));
}

void
pmap_bootstrap(vaddr_t kernend)
{
	uint32_t *pgd = (uint32_t *)PMAP_DIRECT_VA(PGD_PA);
	paddr_t first = PMAP_DIRECT_PA(round_page(kernend));
	u_int i;

	kernel_pmap_store.pm_refcount = 1;
	kernel_pmap_store.pm_pgd = pgd;
	kva_pt = (uint32_t *)PMAP_PT_VA;

	memset(pgd, 0, PAGE_SIZE);
	memset(kva_pt, 0, KVA_NPT * PAGE_SIZE);
	for (i = 0; i < KVA_NPT; i++)
		pgd[KVA_PDE_FIRST + i] = (PT_PA + i * PAGE_SIZE) | PDE_KERNEL;

	__asm volatile("sta %0, [%1] %2" : :
	    "r" (PGD_PA | MMU_PGT_EN | MMU_PGT_FL), "r" (MMU_PGT),
	    "n" (ASI_MMU));

	virtual_avail = VM_MIN_KERNEL_ADDRESS;
	virtual_end = VM_MAX_KERNEL_ADDRESS;

	uvmexp.pagesize = PAGE_SIZE;
	uvm_md_init();
	physmem = atop(RAM_PA_END - first);
	uvm_page_physload(atop(first), atop(RAM_PA_END), atop(first),
	    atop(RAM_PA_END), VM_FREELIST_DEFAULT);
}

vaddr_t
pmap_steal_memory(vsize_t size, vaddr_t *vstartp, vaddr_t *vendp)
{
	const u_int npgs = atop(round_page(size));
	uvm_physseg_t bank;
	vaddr_t va;

	for (bank = uvm_physseg_get_first(); uvm_physseg_valid_p(bank);
	    bank = uvm_physseg_get_next(bank)) {
		const paddr_t pfn = uvm_physseg_get_avail_start(bank);

		if (uvm_physseg_get_avail_end(bank) - pfn < npgs)
			continue;
		if (!uvm_physseg_unplug(pfn, npgs))
			panic("pmap_steal_memory: unplug failed");
		va = PMAP_DIRECT_VA(ptoa(pfn));
		memset((void *)va, 0, round_page(size));
		if (vstartp != NULL)
			*vstartp = virtual_avail;
		if (vendp != NULL)
			*vendp = virtual_end;
		return va;
	}
	panic("pmap_steal_memory: out of memory");
}

void
pmap_init(void)
{
}

void
pmap_virtual_space(vaddr_t *vstartp, vaddr_t *vendp)
{

	*vstartp = virtual_avail;
	*vendp = virtual_end;
}

void
pmap_kenter_pa(vaddr_t va, paddr_t pa, vm_prot_t prot, u_int flags)
{
	uint32_t pte = (pa & PTE_PFN) | PTE_KERNEL;

	if (prot & VM_PROT_WRITE)
		pte |= PTE_W;
	pte_store(va, pte);
}

void
pmap_kremove(vaddr_t va, vsize_t len)
{

	for (; len != 0; va += PAGE_SIZE, len -= PAGE_SIZE)
		pte_store(va, 0);
}

int
pmap_enter(pmap_t pm, vaddr_t va, paddr_t pa, vm_prot_t prot, u_int flags)
{

	KASSERT(pm == pmap_kernel());
	pmap_kenter_pa(va, pa, prot, flags);
	return 0;
}

void
pmap_remove(pmap_t pm, vaddr_t sva, vaddr_t eva)
{

	KASSERT(pm == pmap_kernel());
	pmap_kremove(sva, eva - sva);
}

void
pmap_protect(pmap_t pm, vaddr_t sva, vaddr_t eva, vm_prot_t prot)
{

	KASSERT(pm == pmap_kernel());
	for (; sva < eva; sva += PAGE_SIZE) {
		uint32_t pte = *kvtopte(sva) & ~(PTE_W | PTE_X);

		if (prot & VM_PROT_WRITE)
			pte |= PTE_W;
		if (prot & VM_PROT_EXECUTE)
			pte |= PTE_X;
		pte_store(sva, pte);
	}
}

bool
pmap_extract(pmap_t pm, vaddr_t va, paddr_t *pap)
{
	uint32_t pte;

	KASSERT(pm == pmap_kernel());
	if (va < VM_MIN_KERNEL_ADDRESS) {
		if (pap != NULL)
			*pap = PMAP_DIRECT_PA(va);
		return true;
	}
	pte = *kvtopte(va);
	if ((pte & PTE_V) == 0)
		return false;
	if (pap != NULL)
		*pap = (pte & PTE_PFN) | (va & PAGE_MASK);
	return true;
}

void
pmap_unwire(pmap_t pm, vaddr_t va)
{
}

void
pmap_zero_page(paddr_t pa)
{

	memset((void *)PMAP_DIRECT_VA(pa), 0, PAGE_SIZE);
}

void
pmap_copy_page(paddr_t src, paddr_t dst)
{

	memcpy((void *)PMAP_DIRECT_VA(dst), (void *)PMAP_DIRECT_VA(src),
	    PAGE_SIZE);
}

void
pmap_copy(pmap_t dst, pmap_t src, vaddr_t dva, vsize_t len, vaddr_t sva)
{
}

void
pmap_activate(struct lwp *l)
{
}

void
pmap_deactivate(struct lwp *l)
{
}

/*
 * User pmaps are empty so far: start_init() needs a vmspace to exist before it
 * runs, and pmap_enter() still refuses anything but the kernel pmap.
 */
pmap_t
pmap_create(void)
{
	pmap_t pm = kmem_zalloc(sizeof(*pm), KM_SLEEP);

	pm->pm_refcount = 1;
	return pm;
}

void
pmap_reference(pmap_t pm)
{

	pm->pm_refcount++;
}

void
pmap_destroy(pmap_t pm)
{

	if (--pm->pm_refcount == 0) {
		KASSERT(pm != pmap_kernel());
		kmem_free(pm, sizeof(*pm));
	}
}

/* The rest needs user mappings, which do not exist yet. */
bool
pmap_remove_all(pmap_t pm)
{

	panic("%s: not implemented", __func__);
}

void
pmap_page_protect(struct vm_page *pg, vm_prot_t prot)
{

	panic("%s: not implemented", __func__);
}

void
pmap_pv_protect(paddr_t pa, vm_prot_t prot)
{

	panic("%s: not implemented", __func__);
}

bool
pmap_clear_modify(struct vm_page *pg)
{

	return false;
}

bool
pmap_clear_reference(struct vm_page *pg)
{

	return false;
}

bool
pmap_is_modified(struct vm_page *pg)
{

	return false;
}

bool
pmap_is_referenced(struct vm_page *pg)
{

	return false;
}
