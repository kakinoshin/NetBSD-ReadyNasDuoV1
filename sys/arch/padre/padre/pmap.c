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
#include <sys/atomic.h>
#include <sys/kmem.h>
#include <sys/mutex.h>
#include <sys/pool.h>
#include <sys/proc.h>

#include <uvm/uvm.h>
#include <uvm/uvm_physseg.h>

#define	ASI_MMU		0x10
#define	ASI_TLBPROBE	0x11
#define	MMU_PGT		0x24
#define	MMU_PGT_EN	0x1
#define	MMU_PGT_FL	0x2

#define	ASI_ICMU	0x14
#define	ASI_DCMU	0x15
#define	CMU_CTRL	0x00030000
#define	DCMU_PROBE_CLEAN 2

#define	MBIST_MODE	(*(volatile uint32_t *)0x60000080)
#define	MBIST_ADDR	(*(volatile uint32_t *)0x60000088)
#define	MBIST_DATA	(*(volatile uint32_t *)0x6000008c)
#define	MBIST_MODE_EN	0x001
#define	MBIST_MODE_DONE	0x200

#define	RAM_PA_END	0x7f000000UL	/* cached RAM */
#define	PT_POOL_START	(PMAP_PT_PA + KVA_NPT * PAGE_SIZE)
#define	PT_POOL_END	0x7f400000UL	/* non-cached page tables */
#define	PGD_PA		PMAP_PGD_PA
#define	PT_PA		PMAP_PT_PA
#define	KVA_PDE_FIRST	(VM_MIN_KERNEL_ADDRESS >> PMAP_PGD_SHIFT)
#define	KVA_NPT		(PMAP_NPDE - KVA_PDE_FIRST)

/* The bits Linux uses for pmd_populate_kernel() and PAGE_KERNEL. */
#define	PDE_KERNEL	(PTE_U | PTE_W | PTE_D | PTE_R | PTE_X | PTE_V | PTE_C)
#define	PTE_KERNEL	(PTE_R | PTE_X | PTE_D | PTE_A | PTE_V | PTE_C)

#define	lda(a, asi) ({							\
	uint32_t _v;							\
	__asm volatile("lda [%1] %2, %0" : "=r" (_v) :			\
	    "r" (a), "n" (asi));					\
	_v;								\
})
#define	sta(a, asi, v)							\
	__asm volatile("sta %0, [%1] %2" : : "r" (v), "r" (a), "n" (asi))

#define	VM_PAGE_TO_MD(pg)	(&(pg)->mdpage)

struct pv_entry {
	struct pv_entry	*pv_next;
	pmap_t		pv_pmap;
	vaddr_t		pv_va;
};

static struct pmap kernel_pmap_store;
struct pmap * const kernel_pmap_ptr = &kernel_pmap_store;

static kmutex_t pmap_lock;		/* tables, pv lists, table pool */
static struct pool pv_pool;
static bool pmap_initialized;
static pmap_t curpmap;			/* the tables in MMU_PGT */
static paddr_t pt_next, pt_free_head;

static uint32_t *kva_pt;
static vaddr_t virtual_avail, virtual_end;

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
	kernel_pmap_store.pm_pgd_pa = PGD_PA;
	curpmap = pmap_kernel();
	pt_next = PT_POOL_START;
	mutex_init(&pmap_lock, MUTEX_DEFAULT, IPL_VM);
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

	pool_init(&pv_pool, sizeof(struct pv_entry), 0, 0, 0, "pvpl", NULL,
	    IPL_VM);
	pmap_initialized = true;
}

void
pmap_virtual_space(vaddr_t *vstartp, vaddr_t *vendp)
{

	*vstartp = virtual_avail;
	*vendp = virtual_end;
}

/* Hands out zeroed non-cached pages for tables; 0 means there are none left. */
static paddr_t
pt_alloc(void)
{
	paddr_t pa;

	if (pt_free_head != 0) {
		pa = pt_free_head;
		pt_free_head = *(paddr_t *)PMAP_DIRECT_VA(pa);
	} else if (pt_next < PT_POOL_END) {
		pa = pt_next;
		pt_next += PAGE_SIZE;
	} else
		return 0;
	memset((void *)PMAP_DIRECT_VA(pa), 0, PAGE_SIZE);
	return pa;
}

static void
pt_free(paddr_t pa)
{

	*(paddr_t *)PMAP_DIRECT_VA(pa) = pt_free_head;
	pt_free_head = pa;
}

static inline bool
pmap_is_current(pmap_t pm)
{

	return pm == pmap_kernel() || pm == curpmap;
}

/* Only the current tables can be in the TLB; switching flushes both TLBs. */
static inline void
pte_update(pmap_t pm, vaddr_t va, uint32_t *ptep, uint32_t pte)
{

	if (pmap_is_current(pm))
		pte_store(va, pte);
	else
		*ptep = pte;
}

static uint32_t *
pmap_pte(pmap_t pm, vaddr_t va)
{
	const uint32_t pde = pm->pm_pgd[va >> PMAP_PGD_SHIFT];

	if ((pde & PTE_V) == 0)
		return NULL;
	return (uint32_t *)PMAP_DIRECT_VA(pde & PTE_PFN) +
	    ((va >> PAGE_SHIFT) & (PMAP_NPTE - 1));
}

static void
pv_remove(struct vm_page *pg, pmap_t pm, vaddr_t va, uint32_t opte)
{
	struct vm_page_md * const md = VM_PAGE_TO_MD(pg);
	struct pv_entry **pvp, *pv;

	md->mdpg_attrs |= opte & (PTE_A | PTE_D);
	for (pvp = &md->mdpg_pvh; (pv = *pvp) != NULL; pvp = &pv->pv_next) {
		if (pv->pv_pmap == pm && pv->pv_va == va) {
			*pvp = pv->pv_next;
			pool_put(&pv_pool, pv);
			return;
		}
	}
	panic("%s: no entry for %#lx", __func__, va);
}

static void
pmap_remove_pte(pmap_t pm, vaddr_t va, uint32_t *ptep)
{
	const uint32_t opte = *ptep;
	struct vm_page *pg = pmap_initialized ?
	    PHYS_TO_VM_PAGE(opte & PTE_PFN) : NULL;

	pte_update(pm, va, ptep, 0);
	pm->pm_stats.resident_count--;
	if (pg != NULL)
		pv_remove(pg, pm, va, opte);
}

/*
 * Instructions reach the I-cache only through memory, and the D-cache is
 * write-back. Follows the path Linux runs in padre_cache.c, the _workaround one
 * because nothing sets padre_cachefix: clean the page by probing it, then
 * invalidate the whole I-cache through the MBIST engine with the cache off.
 */
void
pmap_icache_sync(paddr_t pa)
{
	const vaddr_t va = PMAP_DIRECT_VA(trunc_page(pa)), end = va + PAGE_SIZE;
	const u_int psr = getpsr();
	uint32_t dc, ic;
	vaddr_t p;

	setpsr(psr | PSR_PIL);
	dc = lda(CMU_CTRL, ASI_DCMU);
	ic = lda(CMU_CTRL, ASI_ICMU);
	sta(CMU_CTRL, ASI_DCMU, dc | (DCMU_PROBE_CLEAN << 4));
	for (p = va; p < end; p += 16)
		(void)*(volatile uint32_t *)p;
	sta(CMU_CTRL, ASI_DCMU, dc);

	sta(CMU_CTRL, ASI_ICMU, 0);
	__asm volatile("nop; nop; nop");
	MBIST_ADDR = 0x3ff;
	MBIST_DATA = 0;
	MBIST_MODE = MBIST_MODE_EN | (1 << 20);
	while ((MBIST_MODE & MBIST_MODE_DONE) == 0)
		continue;
	MBIST_MODE = 0;
	sta(CMU_CTRL, ASI_ICMU, ic);
	__asm volatile("nop; nop; nop");
	setpsr(psr);
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
	struct vm_page * const pg = pmap_initialized ? PHYS_TO_VM_PAGE(pa) :
	    NULL;
	const u_int pdei = va >> PMAP_PGD_SHIFT;
	struct pv_entry *pv = NULL;
	uint32_t npte = (pa & PTE_PFN) | PTE_V;
	uint32_t *ptep, opte;

	if ((flags & PMAP_NOCACHE) == 0 && pa < RAM_PA_END)
		npte |= PTE_C;
	if (pm != pmap_kernel())
		npte |= PTE_U;
	if (prot & (VM_PROT_READ | VM_PROT_EXECUTE))
		npte |= PTE_R;
	if (prot & VM_PROT_WRITE)
		npte |= PTE_R | PTE_W;
	if (prot & VM_PROT_EXECUTE)
		npte |= PTE_X;

	if (pg != NULL)
		pv = pool_get(&pv_pool, PR_NOWAIT);
	if (pg != NULL && pv == NULL)
		goto nomem;

	mutex_spin_enter(&pmap_lock);
	if ((pm->pm_pgd[pdei] & PTE_V) == 0) {
		const paddr_t pt = pt_alloc();

		if (pt == 0) {
			mutex_spin_exit(&pmap_lock);
			if (pv != NULL)
				pool_put(&pv_pool, pv);
			goto nomem;
		}
		pm->pm_pgd[pdei] = pt | PDE_KERNEL;
	}
	ptep = pmap_pte(pm, va);
	opte = *ptep;
	if ((opte & PTE_V) != 0 && (opte & PTE_PFN) == pa) {
		/* A protection change: keep what hardware has recorded. */
		pte_update(pm, va, ptep, npte | (opte & (PTE_A | PTE_D)));
		mutex_spin_exit(&pmap_lock);
		if (pv != NULL)
			pool_put(&pv_pool, pv);
	} else {
		if (opte & PTE_V)
			pmap_remove_pte(pm, va, ptep);
		if (pg != NULL) {
			struct vm_page_md * const md = VM_PAGE_TO_MD(pg);

			pv->pv_pmap = pm;
			pv->pv_va = va;
			pv->pv_next = md->mdpg_pvh;
			md->mdpg_pvh = pv;
		}
		pte_update(pm, va, ptep, npte);
		pm->pm_stats.resident_count++;
		mutex_spin_exit(&pmap_lock);
	}
	if (prot & VM_PROT_EXECUTE)
		pmap_icache_sync(pa);
	return 0;

nomem:
	if (flags & PMAP_CANFAIL)
		return ENOMEM;
	panic("%s: out of memory", __func__);
}

void
pmap_remove(pmap_t pm, vaddr_t sva, vaddr_t eva)
{
	uint32_t *ptep;

	mutex_spin_enter(&pmap_lock);
	for (; sva < eva; sva += PAGE_SIZE) {
		ptep = pmap_pte(pm, sva);
		if (ptep != NULL && (*ptep & PTE_V) != 0)
			pmap_remove_pte(pm, sva, ptep);
	}
	mutex_spin_exit(&pmap_lock);
}

void
pmap_protect(pmap_t pm, vaddr_t sva, vaddr_t eva, vm_prot_t prot)
{
	uint32_t *ptep, pte;

	if ((prot & VM_PROT_READ) == 0) {
		pmap_remove(pm, sva, eva);
		return;
	}
	mutex_spin_enter(&pmap_lock);
	for (; sva < eva; sva += PAGE_SIZE) {
		ptep = pmap_pte(pm, sva);
		if (ptep == NULL || (*ptep & PTE_V) == 0)
			continue;
		pte = *ptep;
		if ((prot & VM_PROT_WRITE) == 0)
			pte &= ~PTE_W;
		if ((prot & VM_PROT_EXECUTE) == 0)
			pte &= ~PTE_X;
		pte_update(pm, sva, ptep, pte);
	}
	mutex_spin_exit(&pmap_lock);
}

bool
pmap_extract(pmap_t pm, vaddr_t va, paddr_t *pap)
{
	uint32_t *ptep;

	if (pm == pmap_kernel() && va < VM_MIN_KERNEL_ADDRESS) {
		if (pap != NULL)
			*pap = PMAP_DIRECT_PA(va);
		return true;
	}
	ptep = pmap_pte(pm, va);
	if (ptep == NULL || (*ptep & PTE_V) == 0)
		return false;
	if (pap != NULL)
		*pap = (*ptep & PTE_PFN) | (va & PAGE_MASK);
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

static void
pmap_load(pmap_t pm)
{

	curpmap = pm;
	__asm volatile("sta %0, [%1] %2" : :
	    "r" (pm->pm_pgd_pa | MMU_PGT_EN | MMU_PGT_FL), "r" (MMU_PGT),
	    "n" (ASI_MMU));
}

void
pmap_activate(struct lwp *l)
{
	pmap_t pm = l->l_proc->p_vmspace->vm_map.pmap;

	if (pm != curpmap)
		pmap_load(pm);
}

void
pmap_deactivate(struct lwp *l)
{
}

pmap_t
pmap_create(void)
{
	pmap_t pm = kmem_zalloc(sizeof(*pm), KM_SLEEP);

	pm->pm_refcount = 1;
	mutex_spin_enter(&pmap_lock);
	pm->pm_pgd_pa = pt_alloc();
	mutex_spin_exit(&pmap_lock);
	if (pm->pm_pgd_pa == 0)
		panic("%s: out of page tables", __func__);
	pm->pm_pgd = (uint32_t *)PMAP_DIRECT_VA(pm->pm_pgd_pa);
	memcpy(&pm->pm_pgd[KVA_PDE_FIRST],
	    &pmap_kernel()->pm_pgd[KVA_PDE_FIRST], KVA_NPT * sizeof(uint32_t));
	return pm;
}

void
pmap_reference(pmap_t pm)
{

	atomic_inc_uint(&pm->pm_refcount);
}

/* uvm removes every mapping first, so only the tables are left to free. */
void
pmap_destroy(pmap_t pm)
{
	u_int i;

	if (atomic_dec_uint_nv(&pm->pm_refcount) != 0)
		return;
	KASSERT(pm != pmap_kernel());
	if (pm == curpmap)
		pmap_load(pmap_kernel());
	mutex_spin_enter(&pmap_lock);
	for (i = 0; i < KVA_PDE_FIRST; i++) {
		if (pm->pm_pgd[i] & PTE_V)
			pt_free(pm->pm_pgd[i] & PTE_PFN);
	}
	pt_free(pm->pm_pgd_pa);
	mutex_spin_exit(&pmap_lock);
	kmem_free(pm, sizeof(*pm));
}

bool
pmap_remove_all(pmap_t pm)
{

	return false;
}

void
pmap_page_protect(struct vm_page *pg, vm_prot_t prot)
{
	struct vm_page_md * const md = VM_PAGE_TO_MD(pg);
	struct pv_entry *pv;
	uint32_t *ptep;

	if ((prot & (VM_PROT_READ | VM_PROT_WRITE)) ==
	    (VM_PROT_READ | VM_PROT_WRITE))
		return;
	mutex_spin_enter(&pmap_lock);
	if ((prot & VM_PROT_READ) == 0) {
		while ((pv = md->mdpg_pvh) != NULL)
			pmap_remove_pte(pv->pv_pmap, pv->pv_va,
			    pmap_pte(pv->pv_pmap, pv->pv_va));
	} else {
		for (pv = md->mdpg_pvh; pv != NULL; pv = pv->pv_next) {
			ptep = pmap_pte(pv->pv_pmap, pv->pv_va);
			pte_update(pv->pv_pmap, pv->pv_va, ptep,
			    *ptep & ~PTE_W);
		}
	}
	mutex_spin_exit(&pmap_lock);
}

void
pmap_pv_protect(paddr_t pa, vm_prot_t prot)
{

	panic("%s: not implemented", __func__);
}

/* Hardware keeps A and D in the PTEs; removed mappings leave them in the md. */
static bool
pmap_test_attr(struct vm_page *pg, uint32_t bit, bool clear)
{
	struct vm_page_md * const md = VM_PAGE_TO_MD(pg);
	struct pv_entry *pv;
	uint32_t *ptep;
	bool rv;

	mutex_spin_enter(&pmap_lock);
	rv = (md->mdpg_attrs & bit) != 0;
	if (clear)
		md->mdpg_attrs &= ~bit;
	for (pv = md->mdpg_pvh; pv != NULL; pv = pv->pv_next) {
		ptep = pmap_pte(pv->pv_pmap, pv->pv_va);
		if ((*ptep & bit) == 0)
			continue;
		rv = true;
		if (clear)
			pte_update(pv->pv_pmap, pv->pv_va, ptep, *ptep & ~bit);
	}
	mutex_spin_exit(&pmap_lock);
	return rv;
}

bool
pmap_clear_modify(struct vm_page *pg)
{

	return pmap_test_attr(pg, PTE_D, true);
}

bool
pmap_clear_reference(struct vm_page *pg)
{

	return pmap_test_attr(pg, PTE_A, true);
}

bool
pmap_is_modified(struct vm_page *pg)
{

	return pmap_test_attr(pg, PTE_D, false);
}

bool
pmap_is_referenced(struct vm_page *pg)
{

	return pmap_test_attr(pg, PTE_A, false);
}

#define	CHECK(c)	do {						\
	if (!(c))							\
		panic("%s: %s", __func__, #c);				\
} while (0)

/*
 * Bring-up check, from supervisor mode: a user pmap maps a page, hardware
 * records the accesses, and the pv list finds them.
 */
void
pmap_selftest(void)
{
	const vaddr_t va = 0x00400000;
	volatile uint32_t * const uva = (volatile uint32_t *)va;
	struct vm_page *pg = uvm_pagealloc(NULL, 0, NULL, 0);
	const paddr_t pa = VM_PAGE_TO_PHYS(pg);
	volatile uint32_t * const kva =
	    (volatile uint32_t *)PMAP_DIRECT_VA(pa);
	pmap_t pm = pmap_create();
	pmap_t const prev = curpmap;
	paddr_t got;

	*kva = 0x12345678;
	CHECK(pmap_enter(pm, va, pa, VM_PROT_READ | VM_PROT_WRITE |
	    VM_PROT_EXECUTE, PMAP_CANFAIL) == 0);
	CHECK(pmap_extract(pm, va, &got) && got == pa);
	CHECK(!pmap_is_referenced(pg) && !pmap_is_modified(pg));
	pmap_load(pm);
	CHECK(*uva == 0x12345678);
	CHECK(pmap_is_referenced(pg) && !pmap_is_modified(pg));
	*uva = 0x87654321;
	CHECK(*kva == 0x87654321);
	CHECK(pmap_is_modified(pg));
	CHECK(pmap_clear_modify(pg) && !pmap_is_modified(pg));
	*uva = 1;
	CHECK(pmap_is_modified(pg));
	pmap_remove(pm, va, va + PAGE_SIZE);
	CHECK(!pmap_extract(pm, va, NULL));
	CHECK(pmap_is_modified(pg) && pmap_is_referenced(pg));
	pmap_load(prev);
	pmap_destroy(pm);
	uvm_pagefree(pg);
	printf("pmap: selftest passed\n");
}
