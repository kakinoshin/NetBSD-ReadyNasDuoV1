/*	$NetBSD$	*/

/*
 * INTC handling and spl.
 *
 * PSR.PIL is a single global-disable bit and trap levels are fixed per line,
 * so IPLs exist only in software: ipl_mask_[hl] holds, for each IPL, the lines
 * allowed at that IPL, and spl writes those to the INTC.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/cpu.h>
#include <sys/intr.h>
#include <sys/kmem.h>

#include <sys/lwp.h>
#include <sys/proc.h>

#include <machine/frame.h>
#include <machine/reg.h>

#define	INTC_REG(o)	(*(volatile uint32_t *)(0x60000000 + (o)))
#define	INTC_MASK_H	INTC_REG(0x00)
#define	INTC_MASK_L	INTC_REG(0x04)
#define	INTC_FORCE_H	INTC_REG(0x10)
#define	INTC_FORCE_L	INTC_REG(0x14)
#define	INTC_CAUSE_H	INTC_REG(0x18)
#define	INTC_CAUSE_L	INTC_REG(0x1c)

/* Bug 505: a mask write takes effect only after a write to this address. */
#define	INTC_SYNC	(*(volatile uint32_t *)0x66660000)

/* PCI lines are shared, so each line has a list of handlers. */
struct intrhand {
	struct intrhand	*ih_next;
	int	(*ih_fun)(void *);
	void	*ih_arg;
	int	ih_ipl;
};

static struct intrhand *intrhand[NIRQ];
static int intr_ipl[NIRQ];
static uint32_t ipl_mask_h[NIPL], ipl_mask_l[NIPL];
u_int padre_intr_count[NIRQ];	/* bring-up statistics */
struct trapframe *padre_intr_tf;	/* the latest interrupted frame */

static inline void
intc_setmask(int ipl)
{

	INTC_MASK_H = ipl_mask_h[ipl];
	INTC_MASK_L = ipl_mask_l[ipl];
	INTC_SYNC = 0;
}

/* Linux may leave lines enabled and pending when it hands over the CPU. */
void
padre_intr_init(void)
{

	curcpu()->ci_cpl = IPL_HIGH;
	intc_setmask(IPL_HIGH);
	INTC_FORCE_H = 0;
	INTC_FORCE_L = 0;
	INTC_CAUSE_H = 0xffffffff;
	INTC_CAUSE_L = 0xffffffff;
}

bool
cpu_intr_p(void)
{

	return curcpu()->ci_idepth != 0;
}

int
splraiseipl(ipl_cookie_t icookie)
{
	struct cpu_info * const ci = curcpu();
	const int s = ci->ci_cpl;

	if (icookie._ipl > s) {
		ci->ci_cpl = icookie._ipl;
		intc_setmask(icookie._ipl);
	}
	return s;
}

void
splx(int s)
{
	struct cpu_info * const ci = curcpu();

	if (s != ci->ci_cpl) {
		ci->ci_cpl = s;
		intc_setmask(s);
	}
}

/*
 * A NULL arg passes the trapframe to the handler, which is how a clock
 * interrupt gets its clockframe. A shared line runs at the highest IPL of its
 * handlers.
 */
void *
padre_intr_establish(int irq, int ipl, int (*fun)(void *), void *arg)
{
	struct intrhand *ih, **p;
	const uint32_t bit = 1U << (irq & 31);
	int s, i;

	KASSERT(irq >= 0 && irq < NIRQ);
	ih = kmem_zalloc(sizeof(*ih), KM_SLEEP);
	ih->ih_fun = fun;
	ih->ih_arg = arg;
	ih->ih_ipl = ipl;

	s = splhigh();
	for (p = &intrhand[irq]; *p != NULL; p = &(*p)->ih_next)
		continue;
	*p = ih;
	if (ipl > intr_ipl[irq])
		intr_ipl[irq] = ipl;
	for (i = 0; i < NIPL; i++) {
		uint32_t * const m = irq >= 32 ? &ipl_mask_h[i] : &ipl_mask_l[i];

		if (i < intr_ipl[irq])
			*m |= bit;
		else
			*m &= ~bit;
	}
	splx(s);
	return ih;
}

static void
intr_run(struct cpu_info *ci, int irq, struct trapframe *tf)
{
	struct intrhand *ih = intrhand[irq];

	if (ih == NULL) {
		printf("stray interrupt %d\n", irq);
		return;
	}
	padre_intr_count[irq]++;
	ci->ci_cpl = intr_ipl[irq];
	intc_setmask(intr_ipl[irq]);
	for (; ih != NULL; ih = ih->ih_next)
		(*ih->ih_fun)(ih->ih_arg != NULL ? ih->ih_arg : tf);
}

void
padre_intr_dispatch(struct trapframe *tf)
{
	struct cpu_info * const ci = curcpu();
	const int s = ci->ci_cpl;
	uint32_t pend;
	int bit;

	ci->ci_idepth++;
	padre_intr_tf = tf;
	padre_stack_check();

	pend = INTC_CAUSE_H & ipl_mask_h[s];
	INTC_CAUSE_H = pend;
	while ((bit = ffs(pend)) != 0) {
		pend &= ~(1U << (bit - 1));
		intr_run(ci, 32 + bit - 1, tf);
	}

	pend = INTC_CAUSE_L & ipl_mask_l[s];
	INTC_CAUSE_L = pend;
	while ((bit = ffs(pend)) != 0) {
		pend &= ~(1U << (bit - 1));
		intr_run(ci, bit - 1, tf);
	}

	ci->ci_cpl = s;
	intc_setmask(s);
	ci->ci_idepth--;
}

/* Bring-up: the state of the interrupted kernel, for hangs. */
void
padre_debug_dump(const char *why)
{
	struct cpu_info * const ci = curcpu();
	struct lwp * const l = curlwp;
	const struct trapframe * const tf = padre_intr_tf;

	printf("\ndebug (%s): cpl %d idepth %d mtx_count %d lwp %d.%d (%s) "
	    "stat %d syscalls %u\n", why, ci->ci_cpl, ci->ci_idepth,
	    ci->ci_mtx_count, l->l_proc->p_pid, l->l_lid, l->l_proc->p_comm,
	    l->l_stat, (u_int)ci->ci_data.cpu_nsyscall);
	if (tf != NULL)
		printf("debug: interrupted pc %#x npc %#x psr %#x o7 %#x\n",
		    tf->tf_pc, tf->tf_npc, tf->tf_psr, tf->tf_out[7]);
	printf("debug: irq 8 %u, 27 %u, 46 %u\n", padre_intr_count[8],
	    padre_intr_count[27], padre_intr_count[46]);
}
