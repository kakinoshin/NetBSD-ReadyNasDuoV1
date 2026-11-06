/*	$NetBSD$	*/

/*
 * Traps and system calls. Register windows of a user thread are moved to
 * pcb_rw on every entry from user mode, see locore.S. pcb_nsaved counts them,
 * with pcb_rw[0] the window the thread trapped from. Zero means the thread's
 * current window is only on its user stack, at tf_out[6].
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/cpu.h>
#include <sys/kmem.h>
#include <sys/ktrace.h>
#include <sys/mount.h>
#include <sys/lwp.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/signalvar.h>
#include <sys/syscall.h>
#include <sys/syscallargs.h>
#include <sys/syscallvar.h>
#include <sys/userret.h>

#include <uvm/uvm.h>

#include <machine/frame.h>
#include <machine/instr.h>
#include <machine/pcb.h>
#include <machine/trap.h>

#define	ASI_MMU		0x10
#define	MMU_DFSR	0x10
#define	MMU_IFSR	0x14
#define	MMU_DFADDR	0x18
#define	FSR_WRITE	0x200

#define	T_DATAMISS	0x2c
#define	T_TEXTMISS	0x3c

#define	MAXARGS		8

void	trap(u_int, struct trapframe *);
void	trap_userret(struct trapframe *);
void	windows_prepare(struct trapframe *);
int	trap_reg(struct lwp *, struct trapframe *, u_int);
void	fpu_emulate_user(struct lwp *, struct trapframe *);

static inline uint32_t
mmu_read(uint32_t reg)
{
	uint32_t v;

	__asm volatile("lda [%1] %2, %0" : "=r" (v) : "r" (reg), "n" (ASI_MMU));
	return v;
}

static void
windows_fatal(struct lwp *l)
{
	struct trapframe * const tf = l->l_md.md_tf;
	struct pcb * const pcb = lwp_getpcb(l);

	printf("pid %d (%s): bad user stack pc %#x npc %#x sp %#x fp %#x "
	    "uw %d\n", l->l_proc->p_pid, l->l_proc->p_comm, tf->tf_pc,
	    tf->tf_npc, tf->tf_out[6], pcb->pcb_rw[0].rw_in[6], pcb->pcb_uw);
	mutex_enter(l->l_proc->p_lock);
	sigexit(l, SIGSEGV);
}

/* Write the saved windows from "first" up to the thread's stack, oldest first. */
static void
windows_copyout(struct lwp *l, u_int first)
{
	struct pcb * const pcb = lwp_getpcb(l);
	int n = pcb->pcb_nsaved;

	while (n-- > (int)first) {
		const int sp = n != 0 ? pcb->pcb_rw[n - 1].rw_in[6] :
		    l->l_md.md_tf->tf_out[6];

		if (copyout(&pcb->pcb_rw[n], (void *)sp,
		    sizeof(struct rwindow)) != 0) {
			pcb->pcb_nsaved = 0;
			windows_fatal(l);
		}
	}
}

/* Put every window on the user stack, for code that reads it as a frame. */
void
windows_flush(struct lwp *l)
{
	struct pcb * const pcb = lwp_getpcb(l);

	windows_copyout(l, 0);
	pcb->pcb_nsaved = 0;
}

/*
 * The last step before user mode, with traps on. Leaves in pcb_rw only the
 * windows that locore loads: the current one, and for an underflow the one the
 * thread is about to restore into, in pcb_rw[1].
 */
void
windows_prepare(struct trapframe *tf)
{
	struct lwp * const l = curlwp;
	struct pcb * const pcb = lwp_getpcb(l);

	if (pcb->pcb_nsaved == 0) {
		if (copyin((void *)tf->tf_out[6], &pcb->pcb_rw[0],
		    sizeof(struct rwindow)) != 0)
			windows_fatal(l);
	} else {
		windows_copyout(l, 1);
	}
	pcb->pcb_nsaved = 1;
	if (pcb->pcb_uw != 0) {
		pcb->pcb_uw = 0;
		if (copyin((void *)pcb->pcb_rw[0].rw_in[6], &pcb->pcb_rw[1],
		    sizeof(struct rwindow)) != 0)
			windows_fatal(l);
		pcb->pcb_nsaved = 2;
	}
}

/*
 * Bring-up: interrupts run on the stack of whichever lwp they hit, and the
 * pcb sits at the bottom of the uarea, so an overflow first wrecks the saved
 * user windows and then the physical page below the uarea. Reports each new
 * deepest use and panics before the pcb is reached.
 */
void
padre_stack_check(void)
{
	static vsize_t deepest;
	const vaddr_t ua = uvm_lwp_getuarea(curlwp);
	const vaddr_t sp = (vaddr_t)__builtin_frame_address(0);
	vsize_t used;

	if (ua == 0 || sp < ua || sp >= ua + USPACE)
		return;
	used = ua + USPACE - sp;
	if (sp < ua + sizeof(struct pcb) + 512)
		panic("kernel stack overflow: lwp %p sp %#lx uarea %#lx",
		    curlwp, sp, ua);
	if (used > deepest + 256) {
		deepest = used;
		printf("stack: new deepest %lu of %u bytes, lwp %d.%d (%s)\n",
		    (u_long)used, USPACE - (u_int)sizeof(struct pcb),
		    curlwp->l_proc->p_pid, curlwp->l_lid,
		    curlwp->l_proc->p_comm);
	}
}

void
trap_userret(struct trapframe *tf)
{
	struct lwp * const l = curlwp;

	l->l_md.md_tf = tf;
	curcpu()->ci_want_ast = 0;
	mi_userret(l);
}

static void
trap_signal(struct lwp *l, int sig, int code, vaddr_t va, u_int tt)
{
	ksiginfo_t ksi;

	KSI_INIT_TRAP(&ksi);
	ksi.ksi_signo = sig;
	ksi.ksi_code = code;
	ksi.ksi_addr = (void *)va;
	ksi.ksi_trap = tt;
	trapsignal(l, &ksi);
}

static void
trap_panic(u_int tt, struct trapframe *tf, vaddr_t va, uint32_t fsr)
{

	panic("trap %#x pc %#x npc %#x va %#lx fsr %#x", tt, tf->tf_pc,
	    tf->tf_npc, va, fsr);
}

static void	trap_mapdebug(struct lwp *, vaddr_t, bool);
static void	trap_regdebug(struct lwp *, struct trapframe *);

static void
trap_fault(struct lwp *l, u_int tt, struct trapframe *tf, vaddr_t va,
    uint32_t fsr)
{
	const bool user = (tf->tf_psr & PSR_PS) == 0;
	const bool text = tt == T_TEXTMISS || tt == T_TEXTFAULT;
	const vm_prot_t prot = text ? VM_PROT_EXECUTE :
	    (fsr & FSR_WRITE) ? VM_PROT_WRITE : VM_PROT_READ;
	struct pcb * const pcb = lwp_getpcb(l);
	struct vm_map *map;
	void *onfault;
	int error;

	map = va >= VM_MIN_KERNEL_ADDRESS ? kernel_map :
	    &l->l_proc->p_vmspace->vm_map;
	onfault = pcb->pcb_onfault;
	pcb->pcb_onfault = NULL;
	error = (user || va < VM_MAXUSER_ADDRESS ||
	    va >= VM_MIN_KERNEL_ADDRESS) ?
	    uvm_fault(map, trunc_page(va), prot) : EFAULT;
	pcb->pcb_onfault = onfault;
	if (error == 0)
		return;

	if (!user) {
		if (onfault == NULL)
			trap_panic(tt, tf, va, fsr);
		printf("user access fault: pc %#x va %#lx fsr %#x error %d\n",
		    tf->tf_pc, va, fsr, error);
		tf->tf_pc = (int)onfault;
		tf->tf_npc = tf->tf_pc + 4;
		return;
	}
	if (error == ENOMEM) {
		printf("pid %d (%s): out of swap\n", l->l_proc->p_pid,
		    l->l_proc->p_comm);
		trap_signal(l, SIGKILL, SI_NOINFO, va, tt);
	} else {
		printf("pid %d (%s): segv tt %#x pc %#x npc %#x va %#lx fsr %#x "
		    "error %d\n", l->l_proc->p_pid, l->l_proc->p_comm, tt,
		    tf->tf_pc, tf->tf_npc, va, fsr, error);
		trap_mapdebug(l, tf->tf_pc, false);
		trap_regdebug(l, tf);
		trap_signal(l, SIGSEGV, SEGV_MAPERR, va, tt);
	}
}

int
trap_reg(struct lwp *l, struct trapframe *tf, u_int r)
{
	struct pcb * const pcb = lwp_getpcb(l);

	if (r == 0)
		return 0;
	if (r < 8)
		return tf->tf_global[r];
	if (r < 16)
		return tf->tf_out[r - 8];
	if (r < 24)
		return pcb->pcb_rw[0].rw_local[r - 16];
	return pcb->pcb_rw[0].rw_in[r - 24];
}

/* Bring-up: the user registers at a fatal trap. */
static void
trap_regdebug(struct lwp *l, struct trapframe *tf)
{
	struct pcb * const pcb = lwp_getpcb(l);
	const struct rwindow * const rw = &pcb->pcb_rw[0];
	u_int i;

	printf("  nsaved %d psr %#x y %#x\n", pcb->pcb_nsaved, tf->tf_psr,
	    tf->tf_y);
	for (i = 0; i < 8; i++)
		printf("  g%u %08x o%u %08x l%u %08x i%u %08x\n",
		    i, tf->tf_global[i], i, tf->tf_out[i],
		    i, rw->rw_local[i], i, rw->rw_in[i]);
}

/* Bring-up: which mapping holds a failing instruction. */
static void
trap_mapdebug(struct lwp *l, vaddr_t pc, bool dump)
{
	struct vm_map * const map = &l->l_proc->p_vmspace->vm_map;
	struct vm_map_entry *e;
	paddr_t pa = 0;

	vm_map_lock_read(map);
	if (!uvm_map_lookup_entry(map, pc, &e)) {
		vm_map_unlock_read(map);
		printf("  no map entry\n");
		return;
	}
	(void)pmap_extract(map->pmap, pc, &pa);
	printf("  entry %#lx-%#lx off %#llx prot %x/%x obj %p%s amap %p pa %#lx\n",
	    e->start, e->end, (unsigned long long)e->offset, e->protection,
	    e->max_protection, e->object.uvm_obj,
	    (e->object.uvm_obj != NULL &&
	    UVM_OBJ_IS_VNODE(e->object.uvm_obj)) ? " vnode" : "",
	    e->aref.ar_amap, pa);
	if (dump && pa != 0) {
		struct vm_page * const pg = PHYS_TO_VM_PAGE(pa);
		const uint32_t *w = (const uint32_t *)
		    PMAP_DIRECT_VA(trunc_page(pa));
		u_int i;

		/* A wrong page and a corrupted page look the same from user. */
		if (pg != NULL)
			printf("  page obj %p off %#llx want %#llx flags %#x\n",
			    pg->uobject, (unsigned long long)pg->offset,
			    (unsigned long long)(e->offset +
			    trunc_page(pc - e->start)), pg->flags);
		for (i = 0; i < PAGE_SIZE / 4; i += 8)
			printf("  %03x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
			    i * 4, w[i], w[i + 1], w[i + 2], w[i + 3],
			    w[i + 4], w[i + 5], w[i + 6], w[i + 7]);
	}
	vm_map_unlock_read(map);
}

/* flush is not implemented: decode it from the illegal instruction trap. */
static bool
trap_flush(struct lwp *l, struct trapframe *tf)
{
	union instr ins;
	paddr_t pa;
	vaddr_t va;

	if (ufetch_int((void *)tf->tf_pc, &ins.i_int) != 0)
		return false;
	if (ins.i_any.i_op != IOP_reg || ins.i_op3.i_op3 != IOP3_FLUSH)
		return false;
	va = trap_reg(l, tf, ins.i_simm13.i_rs1);
	va += ins.i_simm13.i_i ? ins.i_simm13.i_simm13 :
	    trap_reg(l, tf, ins.i_asi.i_rs2);
	if (pmap_extract(l->l_proc->p_vmspace->vm_map.pmap, va, &pa))
		pmap_icache_sync(pa);
	tf->tf_pc = tf->tf_npc;
	tf->tf_npc += 4;
	return true;
}

static void
syscall(struct lwp *l, struct trapframe *tf)
{
	struct proc * const p = l->l_proc;
	const struct sysent *callp;
	register_t args[MAXARGS], rval[2];
	register_t code = tf->tf_global[1];
	const int new = code & (SYSCALL_G7RFLAG | SYSCALL_G2RFLAG |
	    SYSCALL_G5RFLAG);
	const int opc = tf->tf_pc, onpc = tf->tf_npc;
	int *ap = &tf->tf_out[0];
	int error, n, nap = 6;

	curcpu()->ci_data.cpu_nsyscall++;
	code &= ~(SYSCALL_G7RFLAG | SYSCALL_G2RFLAG | SYSCALL_G5RFLAG);
	if ((new & SYSCALL_G5RFLAG) == SYSCALL_G5RFLAG)
		tf->tf_pc = tf->tf_global[5];
	else if (new & SYSCALL_G2RFLAG)
		tf->tf_pc = tf->tf_global[2];
	else if (new & SYSCALL_G7RFLAG)
		tf->tf_pc = tf->tf_global[7];
	else
		tf->tf_pc = tf->tf_npc;
	tf->tf_npc = tf->tf_pc + 4;

	callp = p->p_emul->e_sysent;
	switch (code) {
	case SYS_syscall:
		code = *ap++;
		nap--;
		break;
	case SYS___syscall:
		if ((p->p_emul->e_flags & EMUL_HAS_SYS___syscall) == 0)
			break;
		code = ap[_QUAD_LOWWORD];
		ap += 2;
		nap -= 2;
		break;
	}
	if ((u_int)code >= p->p_emul->e_nsysent) {
		error = ENOSYS;
		goto bad;
	}
	callp += code;
	n = callp->sy_argsize / sizeof(register_t);
	if (n > nap) {
		KASSERT(n <= MAXARGS);
		error = copyin((char *)tf->tf_out[6] +
		    offsetof(struct frame, fr_argx), &args[nap],
		    (n - nap) * sizeof(*ap));
		if (error != 0)
			goto bad;
		n = nap;
	}
	memcpy(args, ap, n * sizeof(*ap));

	rval[0] = 0;
	rval[1] = tf->tf_out[1];
	error = sy_invoke(callp, l, args, rval, code);

	switch (error) {
	case 0:
		tf->tf_out[0] = rval[0];
		tf->tf_out[1] = rval[1];
		if (!new)
			tf->tf_psr &= ~PSR_C;
		break;
	case ERESTART:
		tf->tf_pc = opc;
		tf->tf_npc = onpc;
		break;
	case EJUSTRETURN:
		break;
	default:
	bad:
		if (p->p_emul->e_errno)
			error = p->p_emul->e_errno[error];
		tf->tf_out[0] = error;
		tf->tf_psr |= PSR_C;
		tf->tf_pc = onpc;
		tf->tf_npc = tf->tf_pc + 4;
		break;
	}
}

/*
 * Entered with PIL up from locore. The fault registers clear on read, so they
 * go first; then PIL drops to what the interrupted code had.
 */
void
trap(u_int tt, struct trapframe *tf)
{
	const uint32_t dfsr = mmu_read(MMU_DFSR);
	const uint32_t ifsr = mmu_read(MMU_IFSR);
	const vaddr_t dfaddr = mmu_read(MMU_DFADDR);
	struct lwp * const l = curlwp;
	struct pcb * const pcb = lwp_getpcb(l);

	setpsr((getpsr() & ~PSR_PIL) | (tf->tf_psr & PSR_PIL));
	padre_stack_check();

	if ((tf->tf_psr & PSR_PS) != 0) {
		switch (tt) {
		case T_DATAMISS:
		case T_DATAFAULT:
			trap_fault(l, tt, tf, dfaddr, dfsr);
			return;
		case T_TEXTMISS:
		case T_TEXTFAULT:
			trap_fault(l, tt, tf, tf->tf_pc, ifsr);
			return;
		}
		trap_panic(tt, tf, dfaddr, dfsr);
	}

	l->l_md.md_tf = tf;
	switch (tt) {
	case T_WINOF:
		break;
	case T_WINUF:
		pcb->pcb_uw = 1;
		break;
	case T_DATAMISS:
	case T_DATAFAULT:
		trap_fault(l, tt, tf, dfaddr, dfsr);
		break;
	case T_TEXTMISS:
	case T_TEXTFAULT:
		trap_fault(l, tt, tf, tf->tf_pc, ifsr);
		break;
	case T_ILLINST:
		if (!trap_flush(l, tf)) {
			int insn = 0;

			(void)ufetch_int((void *)tf->tf_pc, (u_int *)&insn);
			printf("pid %d (%s): illegal insn %08x at pc %#x npc %#x\n",
			    l->l_proc->p_pid, l->l_proc->p_comm, insn, tf->tf_pc,
			    tf->tf_npc);
			trap_mapdebug(l, tf->tf_pc, true);
			trap_signal(l, SIGILL, ILL_ILLOPC, tf->tf_pc, tt);
		}
		break;
	case T_FPDISABLED:
		fpu_emulate_user(l, tf);
		break;
	case T_PRIVINST:
		trap_signal(l, SIGILL, ILL_PRVOPC, tf->tf_pc, tt);
		break;
	case T_ALIGN:
		trap_signal(l, SIGBUS, BUS_ADRALN, dfaddr, tt);
		break;
	case T_SUN_SYSCALL:
		syscall(l, tf);
		break;
	case T_BREAKPOINT:
		trap_signal(l, SIGTRAP, TRAP_BRKPT, tf->tf_pc, tt);
		break;
	case T_DIV0:
		tf->tf_pc = tf->tf_npc;
		tf->tf_npc += 4;
		trap_signal(l, SIGFPE, FPE_INTDIV, tf->tf_pc, tt);
		break;
	case T_FLUSHWIN:
		windows_flush(l);
		tf->tf_pc = tf->tf_npc;
		tf->tf_npc += 4;
		break;
	default:
		trap_signal(l, SIGILL, ILL_ILLTRP, tf->tf_pc, tt);
		break;
	}
	trap_userret(tf);
}

void
md_child_return(struct lwp *l)
{

	trap_userret(l->l_md.md_tf);
}

void
cpu_spawn_return(struct lwp *l)
{

	trap_userret(l->l_md.md_tf);
}

void
startlwp(void *arg)
{
	ucontext_t * const uc = arg;
	struct lwp * const l = curlwp;
	int error __diagused;

	error = cpu_setmcontext(l, &uc->uc_mcontext, uc->uc_flags);
	KASSERT(error == 0);
	kmem_free(uc, sizeof(ucontext_t));
	trap_userret(l->l_md.md_tf);
}

int
sys_sysarch(struct lwp *l, const struct sys_sysarch_args *uap,
    register_t *retval)
{

	return EINVAL;
}
