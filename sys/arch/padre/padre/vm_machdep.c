/*	$NetBSD$	*/

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/buf.h>
#include <sys/cpu.h>
#include <sys/lwp.h>
#include <sys/proc.h>

#include <uvm/uvm_extern.h>
#include <uvm/uvm_page.h>

#include <machine/frame.h>
#include <machine/pcb.h>

void	lwp_trampoline(void);

/*
 * Build a stack that cpu_switchto() can return through into lwp_trampoline,
 * below a copy of the parent's trapframe at the top of the uarea.
 *
 * A forked child takes a copy of the parent's saved windows. They cannot go to
 * the stack first: the address space is already copied, so a write from here
 * reaches only the parent's page. An LWP or a child on a new stack starts with
 * no windows and reloads from its stack.
 */
void
cpu_lwp_fork(struct lwp *l1, struct lwp *l2, void *stack, size_t stacksize,
    void (*func)(void *), void *arg)
{
	struct pcb * const pcb = lwp_getpcb(l2);
	struct trapframe * const tf = (struct trapframe *)
	    (uvm_lwp_getuarea(l2) + USPACE - sizeof(struct trapframe));
	struct rwindow * const rw = (struct rwindow *)
	    ((char *)tf - sizeof(struct frame));

	memset(pcb, 0, sizeof(*pcb));
	if (l1 == curlwp && l1->l_proc != l2->l_proc && stack == NULL) {
		const struct pcb * const pcb1 = lwp_getpcb(l1);

		pcb->pcb_nsaved = pcb1->pcb_nsaved;
		memcpy(pcb->pcb_rw, pcb1->pcb_rw, sizeof(pcb->pcb_rw));
	}
	memset(rw, 0, sizeof(struct frame));
	*tf = *l1->l_md.md_tf;
	l2->l_md.md_fpstate = NULL;
	if (l1->l_md.md_fpstate != NULL) {
		l2->l_md.md_fpstate = kmem_alloc(sizeof(struct fpstate),
		    KM_SLEEP);
		memcpy(l2->l_md.md_fpstate, l1->l_md.md_fpstate,
		    sizeof(struct fpstate));
	}
	l2->l_md.md_tf = tf;
	if (stack != NULL)
		tf->tf_out[6] = (int)stack + stacksize;
	tf->tf_psr &= ~PSR_C;
	tf->tf_out[0] = 0;
	tf->tf_out[1] = 1;
	rw->rw_local[0] = (int)func;
	rw->rw_local[1] = (int)arg;
	pcb->pcb_sp = (int)rw;
	/* cpu_switchto() returns to this address + 8, as from a call. */
	pcb->pcb_pc = (int)lwp_trampoline - 8;
}

void
cpu_lwp_free(struct lwp *l, int proc)
{
}

void
cpu_lwp_free2(struct lwp *l)
{

	if (l->l_md.md_fpstate != NULL) {
		kmem_free(l->l_md.md_fpstate, sizeof(struct fpstate));
		l->l_md.md_fpstate = NULL;
	}
}

/*
 * The window handlers run with traps off and a TLB miss there resets the CPU,
 * so a kernel stack must never fall back to the translated kernel window.
 */
void *
cpu_uarea_alloc(bool system)
{
	struct pglist pglist;
	int error;

	error = uvm_pglistalloc(USPACE, 0, ~(paddr_t)0, USPACE, 0, &pglist,
	    1, 1);
	if (error != 0)
		panic("%s: %d", __func__, error);
	return (void *)PMAP_DIRECT_VA(VM_PAGE_TO_PHYS(TAILQ_FIRST(&pglist)));
}

bool
cpu_uarea_free(void *va)
{
	paddr_t pa = PMAP_DIRECT_PA(va);
	const paddr_t end = pa + USPACE;

	for (; pa < end; pa += PAGE_SIZE)
		uvm_pagefree(PHYS_TO_VM_PAGE(pa));
	return true;
}

/* Maps the user pages of a physio buffer into phys_map. */
int
vmapbuf(struct buf *bp, vsize_t len)
{
	struct pmap * const upmap = vm_map_pmap(&bp->b_proc->p_vmspace->vm_map);
	vaddr_t faddr, taddr, off;
	paddr_t pa;

	KASSERT((bp->b_flags & B_PHYS) != 0);
	bp->b_saveaddr = bp->b_data;
	faddr = trunc_page((vaddr_t)bp->b_data);
	off = (vaddr_t)bp->b_data - faddr;
	len = round_page(off + len);
	taddr = uvm_km_alloc(phys_map, len, 0, UVM_KMF_VAONLY | UVM_KMF_WAITVA);
	bp->b_data = (void *)(taddr + off);
	for (; len > 0; len -= PAGE_SIZE, faddr += PAGE_SIZE, taddr += PAGE_SIZE) {
		if (!pmap_extract(upmap, faddr, &pa))
			panic("%s: no page at %#lx", __func__, faddr);
		pmap_enter(pmap_kernel(), taddr, pa, VM_PROT_READ | VM_PROT_WRITE,
		    VM_PROT_READ | VM_PROT_WRITE | PMAP_WIRED);
	}
	pmap_update(pmap_kernel());
	return 0;
}

void
vunmapbuf(struct buf *bp, vsize_t len)
{
	vaddr_t addr, off;

	KASSERT((bp->b_flags & B_PHYS) != 0);
	addr = trunc_page((vaddr_t)bp->b_data);
	off = (vaddr_t)bp->b_data - addr;
	len = round_page(off + len);
	pmap_remove(pmap_kernel(), addr, addr + len);
	pmap_update(pmap_kernel());
	uvm_km_free(phys_map, addr, len, UVM_KMF_VAONLY);
	bp->b_data = bp->b_saveaddr;
	bp->b_saveaddr = NULL;
}
