/*	$NetBSD$	*/

#ifndef _PADRE_PMAP_H_
#define _PADRE_PMAP_H_

/* Kernel page tables, in the non-cached top 16 MiB of RAM. */
#define	PMAP_PGD_PA	0x7f000000
#define	PMAP_PT_PA	0x7f001000
#define	PMAP_PT_VA	0xbf001000

#ifndef _LOCORE

#include <uvm/uvm_pmap.h>

/* IT3107 PTE. PDEs use the same layout and point at a 4 KiB table. */
#define	PTE_U		0x200		/* user */
#define	PTE_R		0x100
#define	PTE_W		0x080
#define	PTE_X		0x040
#define	PTE_C		0x020		/* cacheable */
#define	PTE_T		0x010
#define	PTE_A		0x008
#define	PTE_D		0x004
#define	PTE_Z		0x002
#define	PTE_V		0x001
#define	PTE_PFN		0xfffff000

#define	PMAP_PGD_SHIFT	22
#define	PMAP_NPDE	1024
#define	PMAP_NPTE	1024

#define	PMAP_DIRECT_PA(va)	((paddr_t)(va) - 0x40000000)
#define	PMAP_DIRECT_VA(pa)	((vaddr_t)(pa) + 0x40000000)

/* Pool pages come straight from the direct map. */
#define	PMAP_MAP_POOLPAGE(pa)		PMAP_DIRECT_VA(pa)
#define	PMAP_UNMAP_POOLPAGE(va)		PMAP_DIRECT_PA(va)

#define	PMAP_STEAL_MEMORY

struct pmap {
	u_int			pm_refcount;
	uint32_t		*pm_pgd;
	struct pmap_statistics	pm_stats;
};

#define	pmap_resident_count(pm)		((pm)->pm_stats.resident_count)
#define	pmap_wired_count(pm)		((pm)->pm_stats.wired_count)
#define	pmap_phys_address(x)		(x)
#define	pmap_update(pm)			/* nothing */

void	pmap_bootstrap(vaddr_t);

#endif /* !_LOCORE */

#endif /* _PADRE_PMAP_H_ */
