/*	$NetBSD$	*/

/*
 * The IT3107 has no FPU, so PSR.EF stays clear and every FP instruction traps
 * as T_FPDISABLED. The arithmetic goes to the sparc soft-float code in
 * sys/arch/sparc/fpu; loads, stores and FBfcc are done here. A run of FP
 * instructions is emulated in one trap.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/lwp.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/signalvar.h>

#include <machine/instr.h>
#include <machine/reg.h>
#include <machine/trap.h>

#include <sparc/fpu/fpu_emu.h>
#include <sparc/fpu/fpu_extern.h>

#define	FPU_RUN_MAX	64

/* fpu_execute() results, private to fpu.c. */
#define	FPE		1
#define	NOTFPU		2

/* FBfcc conditions: bit n set means taken when fcc is n (=, <, >, ?). */
static const uint8_t fbfcc_taken[16] = {
	0x0, 0xe, 0x6, 0xa, 0x2, 0xc, 0x4, 0x8,
	0xf, 0x1, 0x9, 0x5, 0xd, 0x3, 0xb, 0x7,
};

int	trap_reg(struct lwp *, struct trapframe *, u_int);
void	fpu_emulate_user(struct lwp *, struct trapframe *);

static void
fpu_signal(struct lwp *l, struct trapframe *tf, int sig, int code)
{
	ksiginfo_t ksi;

	KSI_INIT_TRAP(&ksi);
	ksi.ksi_signo = sig;
	ksi.ksi_code = code;
	ksi.ksi_addr = (void *)tf->tf_pc;
	ksi.ksi_trap = T_FPDISABLED;
	trapsignal(l, &ksi);
}

static inline void
fpu_step(struct trapframe *tf)
{

	tf->tf_pc = tf->tf_npc;
	tf->tf_npc += 4;
}

/* Returns 0 when the instruction was emulated, or a signal to post. */
static int
fpu_one(struct lwp *l, struct trapframe *tf, struct fpstate *fs,
    union instr ins)
{
	struct fpemu fe;
	vaddr_t va;
	u_int *reg;
	int error;

	switch (ins.i_any.i_op) {
	case IOP_reg:
		if (ins.i_op3.i_op3 != IOP3_FPop1 && ins.i_op3.i_op3 != IOP3_FPop2)
			return -1;
		fe.fe_fpstate = fs;
		error = fpu_execute(&fe, ins);
		if (error == NOTFPU)
			return SIGILL;
		if (error == FPE)
			return SIGFPE;
		fpu_step(tf);
		return 0;

	case IOP_mem:
		va = trap_reg(l, tf, ins.i_simm13.i_rs1) +
		    (ins.i_simm13.i_i ? ins.i_simm13.i_simm13 :
		    trap_reg(l, tf, ins.i_asi.i_rs2));
		reg = &fs->fs_regs[ins.i_op3.i_rd];
		switch (ins.i_op3.i_op3) {
		case IOP3_LDF:
		case IOP3_STF:
		case IOP3_LDFSR:
		case IOP3_STFSR:
			if (va & 3)
				return SIGBUS;
			break;
		case IOP3_LDDF:
		case IOP3_STDF:
			if ((va & 7) || (ins.i_op3.i_rd & 1))
				return SIGBUS;
			break;
		default:
			return -1;
		}
		switch (ins.i_op3.i_op3) {
		case IOP3_LDF:
			error = copyin((void *)va, reg, 4);
			break;
		case IOP3_LDDF:
			error = copyin((void *)va, reg, 8);
			break;
		case IOP3_LDFSR:
			error = copyin((void *)va, &fs->fs_fsr, 4);
			break;
		case IOP3_STF:
			error = copyout(reg, (void *)va, 4);
			break;
		case IOP3_STDF:
			error = copyout(reg, (void *)va, 8);
			break;
		default:
			error = copyout(&fs->fs_fsr, (void *)va, 4);
			break;
		}
		if (error != 0)
			return SIGSEGV;
		fpu_step(tf);
		return 0;

	case IOP_OP2:
		if (ins.i_branch.i_op2 != IOP2_FBfcc)
			return -1;
		{
			const u_int fcc = (fs->fs_fsr >> FSR_FCC_SHIFT) &
			    FSR_FCC_MASK;
			const u_int cond = ins.i_branch.i_cond;
			const bool taken = (fbfcc_taken[cond] >> fcc) & 1;
			const int target = tf->tf_pc + ins.i_branch.i_disp * 4;

			if (taken && (cond != 8 || !ins.i_branch.i_annul)) {
				tf->tf_pc = tf->tf_npc;
				tf->tf_npc = target;
			} else if (taken) {
				tf->tf_pc = target;
				tf->tf_npc = target + 4;
			} else if (ins.i_branch.i_annul) {
				tf->tf_pc = tf->tf_npc + 4;
				tf->tf_npc = tf->tf_pc + 4;
			} else
				fpu_step(tf);
		}
		return 0;
	}
	return -1;
}

void
fpu_emulate_user(struct lwp *l, struct trapframe *tf)
{
	struct fpstate *fs = l->l_md.md_fpstate;
	union instr ins;
	int n, sig;

	if (fs == NULL) {
		fs = kmem_zalloc(sizeof(*fs), KM_SLEEP);
		l->l_md.md_fpstate = fs;
	}
	for (n = 0; n < FPU_RUN_MAX; n++) {
		if (ufetch_int((void *)tf->tf_pc, &ins.i_int) != 0) {
			if (n == 0)
				fpu_signal(l, tf, SIGSEGV, SEGV_MAPERR);
			return;
		}
		sig = fpu_one(l, tf, fs, ins);
		if (sig < 0) {
			/* Not FP: the first instruction trapped, so it must be. */
			if (n == 0) {
				printf("pid %d (%s): not an FP insn %08x at pc %#x\n",
				    l->l_proc->p_pid, l->l_proc->p_comm, ins.i_int,
				    tf->tf_pc);
				fpu_signal(l, tf, SIGILL, ILL_ILLOPC);
			}
			return;
		}
		if (sig != 0) {
			printf("pid %d (%s): FP insn %08x at pc %#x: signal %d\n",
			    l->l_proc->p_pid, l->l_proc->p_comm, ins.i_int,
			    tf->tf_pc, sig);
			fpu_signal(l, tf, sig, sig == SIGFPE ? FPE_FLTINV :
			    sig == SIGBUS ? BUS_ADRALN : 0);
			return;
		}
	}
}
