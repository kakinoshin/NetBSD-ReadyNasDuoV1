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

struct intrhand {
	int	(*ih_fun)(void *);
	void	*ih_arg;
	int	ih_ipl;
};

static struct intrhand intrhand[NIRQ];
static uint32_t ipl_mask_h[NIPL], ipl_mask_l[NIPL];

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
 * interrupt gets its clockframe.
 */
void *
padre_intr_establish(int irq, int ipl, int (*fun)(void *), void *arg)
{
	struct intrhand *ih = &intrhand[irq];
	const uint32_t bit = 1U << (irq & 31);
	int s, i;

	KASSERT(irq >= 0 && irq < NIRQ);
	KASSERT(ih->ih_fun == NULL);

	s = splhigh();
	ih->ih_fun = fun;
	ih->ih_arg = arg;
	ih->ih_ipl = ipl;
	for (i = 0; i < ipl; i++) {
		if (irq >= 32)
			ipl_mask_h[i] |= bit;
		else
			ipl_mask_l[i] |= bit;
	}
	splx(s);
	return ih;
}

static void
intr_run(struct cpu_info *ci, int irq, struct trapframe *tf)
{
	struct intrhand *ih = &intrhand[irq];

	if (ih->ih_fun == NULL) {
		printf("stray interrupt %d\n", irq);
		return;
	}
	ci->ci_cpl = ih->ih_ipl;
	intc_setmask(ih->ih_ipl);
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
