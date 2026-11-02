/*	$NetBSD$	*/

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
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
