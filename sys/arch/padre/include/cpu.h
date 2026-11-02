/*	$NetBSD$	*/

#ifndef _PADRE_CPU_H_
#define _PADRE_CPU_H_

/* CTL_MACHDEP names, shared with NetBSD/sparc userland. */
#define	CPU_BOOTED_KERNEL	1	/* string: booted kernel name */
#define	CPU_BOOTED_DEVICE	2	/* string: device booted from */
#define	CPU_BOOT_ARGS		3	/* string: args booted with */
#define	CPU_ARCH		4	/* integer: always 7, see port plan */

#if defined(_KERNEL) || defined(_KMEMUSER)

#include <sys/cpu_data.h>
#include <sys/evcnt.h>

#include <machine/intr.h>
#include <machine/psl.h>

struct cpu_info {
	struct cpu_data	ci_data;	/* MI per-cpu data */
	struct cpu_info	*ci_self;
	struct lwp	*ci_curlwp;
	struct lwp	*ci_onproc;
	struct pcb	*curpcb;
	struct lwp	*fplwp;		/* never set: there is no FPU */
	int		ci_cpuid;
	int		ci_cpl;		/* current software IPL */
	int		ci_idepth;	/* interrupt nesting depth */
	int		ci_want_ast;
	int		ci_want_resched;
	int		ci_mtx_count;
	int		ci_mtx_oldspl;
};

#define	CPU_INFO_ITERATOR	int
#define	CPU_INFO_FOREACH(cii, ci) \
	(void)(cii), ci = &cpu_info_store; ci != NULL; ci = NULL

#endif /* _KERNEL || _KMEMUSER */

#ifdef _KERNEL

extern struct cpu_info cpu_info_store;

#define	curcpu()		(&cpu_info_store)
#define	cpuinfo			cpu_info_store
#define	cpu_number()		0
#define	curlwp			(cpu_info_store.ci_curlwp)
#define	curpcb			((struct pcb *)lwp_getpcb(curlwp))
#define	LWP0_CPU_INFO		(&cpu_info_store)

struct clockframe {
	u_int	pc;
	u_int	psr;
};

#define	CLKF_USERMODE(cf)	(((cf)->psr & PSR_PS) == 0)
#define	CLKF_PC(cf)		((cf)->pc)
#define	CLKF_INTR(cf)		(curcpu()->ci_idepth > 1)

#define	LWP_PC(l)		0

void	cpu_signotify(struct lwp *);
#define	cpu_need_proftick(l)	cpu_signotify(l)

#define	cpu_proc_fork(p1, p2)	/* nothing */

struct trapframe;

void	windows_flush(struct lwp *);
void	*cpu_uarea_alloc(bool);
bool	cpu_uarea_free(void *);

#endif /* _KERNEL */

#endif /* _PADRE_CPU_H_ */
