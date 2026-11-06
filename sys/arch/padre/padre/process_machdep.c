/*	$NetBSD$	*/

/*
 * Register access for ptrace and core dumps, after sparc's process_machdep.c.
 * struct reg has the layout of struct trapframe. There is no FPU, so the FP
 * registers are the emulator's state, or zeros before the first FP use.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/lwp.h>
#include <sys/proc.h>
#include <sys/ptrace.h>

#include <machine/frame.h>
#include <machine/psl.h>
#include <machine/reg.h>

int
process_read_regs(struct lwp *l, struct reg *regs)
{

	memcpy(regs, l->l_md.md_tf, sizeof(*regs));
	return 0;
}

int
process_write_regs(struct lwp *l, const struct reg *regs)
{
	const int psr = l->l_md.md_tf->tf_psr & ~PSR_ICC;

	if (((regs->r_pc | regs->r_npc) & 0x03) != 0)
		return EINVAL;
	memcpy(l->l_md.md_tf, regs, sizeof(*regs));
	l->l_md.md_tf->tf_psr = psr | (regs->r_psr & PSR_ICC);
	return 0;
}

int
process_set_pc(struct lwp *l, void *addr)
{

	if (((u_int)addr & 0x03) != 0)
		return EINVAL;
	l->l_md.md_tf->tf_pc = (u_int)addr;
	l->l_md.md_tf->tf_npc = (u_int)addr + 4;
	return 0;
}

int
process_read_fpregs(struct lwp *l, struct fpreg *regs, size_t *sz)
{
	const struct fpstate * const fs = l->l_md.md_fpstate;

	if (fs != NULL)
		*regs = fs->fs_reg;
	else
		memset(regs, 0, sizeof(*regs));
	return 0;
}

int
process_write_fpregs(struct lwp *l, const struct fpreg *regs, size_t sz)
{
	struct fpstate *fs = l->l_md.md_fpstate;

	if (fs == NULL) {
		fs = kmem_zalloc(sizeof(*fs), KM_SLEEP);
		l->l_md.md_fpstate = fs;
	} else
		fs->fs_qsize = 0;
	fs->fs_reg = *regs;
	return 0;
}
