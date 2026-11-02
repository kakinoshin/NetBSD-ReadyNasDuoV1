/*	$NetBSD$	*/

#ifndef _PADRE_VMPARAM_H_
#define _PADRE_VMPARAM_H_

#define	__USE_TOPDOWN_VM

#define	PAGE_SHIFT		12
#define	PAGE_SIZE		(1 << PAGE_SHIFT)
#define	PAGE_MASK		(PAGE_SIZE - 1)

#define	MIN_PAGE_SIZE		PAGE_SIZE
#define	MAX_PAGE_SIZE		PAGE_SIZE

/* User space is the bottom 1 GiB; the MMU translates nothing above it. */
#define	USRSTACK	VM_MAXUSER_ADDRESS

#ifndef MAXTSIZ
#define	MAXTSIZ		(64*1024*1024)		/* max text size */
#endif
#ifndef DFLDSIZ
#define	DFLDSIZ		(64*1024*1024)		/* initial data size limit */
#endif
#ifndef MAXDSIZ
#define	MAXDSIZ		(512*1024*1024)		/* max data size */
#endif
#ifndef DFLSSIZ
#define	DFLSSIZ		(8*1024*1024)		/* initial stack size limit */
#endif
#ifndef MAXSSIZ
#define	MAXSSIZ		(32*1024*1024)		/* max stack size */
#endif

#define	VM_MIN_ADDRESS		((vaddr_t)0)
#define	VM_MAX_ADDRESS		((vaddr_t)0x40000000)
#define	VM_MAXUSER_ADDRESS	((vaddr_t)0x40000000)
#define	VM_MIN_KERNEL_ADDRESS	((vaddr_t)0xf8000000)
#define	VM_MAX_KERNEL_ADDRESS	((vaddr_t)0xffff0000)

#define	VM_PHYSSEG_MAX		2
#define	VM_PHYSSEG_STRAT	VM_PSTRAT_BSEARCH

struct pv_entry;

/* Accesses that hardware recorded in PTEs that are gone hold in mdpg_attrs. */
struct vm_page_md {
	struct pv_entry	*mdpg_pvh;
	u_int		mdpg_attrs;
};

#define	VM_MDPAGE_INIT(pg)						\
do {									\
	(pg)->mdpage.mdpg_pvh = NULL;					\
	(pg)->mdpage.mdpg_attrs = 0;					\
} while (0)

#define	VM_NFREELIST		1
#define	VM_FREELIST_DEFAULT	0

#endif /* _PADRE_VMPARAM_H_ */
