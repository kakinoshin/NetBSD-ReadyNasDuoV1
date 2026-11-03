/*	$NetBSD$	*/

/*
 * Exec and signal state, from sys/arch/sparc/sparc/machdep.c without the FPU.
 * NetBSD/sparc binaries see the same mcontext and signal frame.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/cpu.h>
#include <sys/exec.h>
#include <sys/kmem.h>
#include <sys/lwp.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/signalvar.h>
#include <sys/ucontext.h>

#include <machine/frame.h>
#include <machine/pcb.h>

struct sigframe {
	siginfo_t sf_si;
	ucontext_t sf_uc;
};

void
setregs(struct lwp *l, struct exec_package *pack, vaddr_t stack)
{
	struct trapframe * const tf = l->l_md.md_tf;
	struct pcb * const pcb = lwp_getpcb(l);

	l->l_proc->p_md.md_flags &= ~MDP_FIXALIGN;
	if (l->l_md.md_fpstate != NULL) {
		kmem_free(l->l_md.md_fpstate, sizeof(struct fpstate));
		l->l_md.md_fpstate = NULL;
	}
	memset(tf, 0, sizeof(*tf));
	tf->tf_global[1] = l->l_proc->p_psstrp;
	tf->tf_pc = pack->ep_entry & ~3;
	tf->tf_npc = tf->tf_pc + 4;
	tf->tf_out[6] = stack - sizeof(struct rwindow);
	pcb->pcb_nsaved = 0;
	pcb->pcb_uw = 0;
}

void
sendsig_siginfo(const ksiginfo_t *ksi, const sigset_t *mask)
{
	struct lwp * const l = curlwp;
	struct proc * const p = l->l_proc;
	struct sigacts * const ps = p->p_sigacts;
	struct trapframe * const tf = l->l_md.md_tf;
	const int sig = ksi->ksi_signo;
	const u_int catcher = (u_int)SIGACTION(p, sig).sa_handler;
	const u_int oldsp = tf->tf_out[6];
	const bool onstack =
	    (l->l_sigstk.ss_flags & (SS_DISABLE | SS_ONSTACK)) == 0 &&
	    (SIGACTION(p, sig).sa_flags & SA_ONSTACK) != 0;
	struct sigframe *fp;
	ucontext_t uc;
	u_int newsp;
	size_t ucsz;
	int error;

	if (onstack)
		fp = (struct sigframe *)((char *)l->l_sigstk.ss_sp +
		    l->l_sigstk.ss_size);
	else
		fp = (struct sigframe *)oldsp;
	fp = (struct sigframe *)((int)(fp - 1) & ~STACK_ALIGNBYTES);

	uc.uc_flags = _UC_SIGMASK | ((l->l_sigstk.ss_flags & SS_ONSTACK) ?
	    _UC_SETSTACK : _UC_CLRSTACK);
	uc.uc_sigmask = *mask;
	uc.uc_link = l->l_ctxlink;
	memset(&uc.uc_stack, 0, sizeof(uc.uc_stack));

	sendsig_reset(l, sig);
	mutex_exit(p->p_lock);
	newsp = (int)fp - sizeof(struct frame);
	cpu_getmcontext(l, &uc.uc_mcontext, &uc.uc_flags);
	ucsz = (int)&uc.__uc_pad - (int)&uc;
	error = (copyout(&ksi->ksi_info, &fp->sf_si, sizeof(ksi->ksi_info)) ||
	    copyout(&uc, &fp->sf_uc, ucsz) ||
	    ustore_int((u_int *)&((struct rwindow *)newsp)->rw_in[6], oldsp));
	mutex_enter(p->p_lock);
	if (error)
		sigexit(l, SIGILL);

	switch (ps->sa_sigdesc[sig].sd_vers) {
	case __SIGTRAMP_SIGINFO_VERSION:
		tf->tf_pc = catcher;
		tf->tf_npc = catcher + 4;
		tf->tf_out[0] = sig;
		tf->tf_out[1] = (int)&fp->sf_si;
		tf->tf_out[2] = (int)&fp->sf_uc;
		tf->tf_out[6] = newsp;
		tf->tf_out[7] = (int)ps->sa_sigdesc[sig].sd_tramp - 8;
		break;
	default:
		sigexit(l, SIGILL);
	}
	if (onstack)
		l->l_sigstk.ss_flags |= SS_ONSTACK;
}

void
cpu_getmcontext(struct lwp *l, mcontext_t *mcp, unsigned int *flags)
{
	struct trapframe * const tf = l->l_md.md_tf;
	__greg_t * const r = mcp->__gregs;

	windows_flush(l);
	r[_REG_PSR] = tf->tf_psr;
	r[_REG_PC] = tf->tf_pc;
	r[_REG_nPC] = tf->tf_npc;
	r[_REG_Y] = tf->tf_y;
	r[_REG_G1] = tf->tf_global[1];
	r[_REG_G2] = tf->tf_global[2];
	r[_REG_G3] = tf->tf_global[3];
	r[_REG_G4] = tf->tf_global[4];
	r[_REG_G5] = tf->tf_global[5];
	r[_REG_G6] = tf->tf_global[6];
	r[_REG_G7] = tf->tf_global[7];
	r[_REG_O0] = tf->tf_out[0];
	r[_REG_O1] = tf->tf_out[1];
	r[_REG_O2] = tf->tf_out[2];
	r[_REG_O3] = tf->tf_out[3];
	r[_REG_O4] = tf->tf_out[4];
	r[_REG_O5] = tf->tf_out[5];
	r[_REG_O6] = tf->tf_out[6];
	r[_REG_O7] = tf->tf_out[7];
	*flags |= _UC_CPU | _UC_TLSBASE;
}

int
cpu_mcontext_validate(struct lwp *l, const mcontext_t *mc)
{
	const __greg_t * const gr = mc->__gregs;

	/* Only the icc bits of the psr are used. */
	if (((gr[_REG_PC] | gr[_REG_nPC]) & 3) != 0 ||
	    gr[_REG_PC] == 0 || gr[_REG_nPC] == 0)
		return EINVAL;
	return 0;
}

int
cpu_setmcontext(struct lwp *l, const mcontext_t *mcp, unsigned int flags)
{
	struct trapframe * const tf = l->l_md.md_tf;
	const __greg_t * const r = mcp->__gregs;
	struct proc * const p = l->l_proc;
	int error;

	windows_flush(l);
	if (flags & _UC_CPU) {
		error = cpu_mcontext_validate(l, mcp);
		if (error)
			return error;
		tf->tf_psr = (tf->tf_psr & ~PSR_ICC) | (r[_REG_PSR] & PSR_ICC);
		tf->tf_pc = r[_REG_PC];
		tf->tf_npc = r[_REG_nPC];
		tf->tf_y = r[_REG_Y];
		tf->tf_global[1] = r[_REG_G1];
		tf->tf_global[2] = r[_REG_G2];
		tf->tf_global[3] = r[_REG_G3];
		tf->tf_global[4] = r[_REG_G4];
		tf->tf_global[5] = r[_REG_G5];
		tf->tf_global[6] = r[_REG_G6];
		tf->tf_out[0] = r[_REG_O0];
		tf->tf_out[1] = r[_REG_O1];
		tf->tf_out[2] = r[_REG_O2];
		tf->tf_out[3] = r[_REG_O3];
		tf->tf_out[4] = r[_REG_O4];
		tf->tf_out[5] = r[_REG_O5];
		tf->tf_out[6] = r[_REG_O6];
		tf->tf_out[7] = r[_REG_O7];
		if (flags & _UC_TLSBASE)
			lwp_setprivate(l, (void *)(uintptr_t)r[_REG_G7]);
	}
	mutex_enter(p->p_lock);
	if (flags & _UC_SETSTACK)
		l->l_sigstk.ss_flags |= SS_ONSTACK;
	if (flags & _UC_CLRSTACK)
		l->l_sigstk.ss_flags &= ~SS_ONSTACK;
	mutex_exit(p->p_lock);
	return 0;
}

int
cpu_lwp_setprivate(lwp_t *l, void *addr)
{

	l->l_md.md_tf->tf_global[7] = (uintptr_t)addr;
	return 0;
}
