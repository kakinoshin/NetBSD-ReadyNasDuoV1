/*	$NetBSD$	*/

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/cpu.h>
#include <sys/intr.h>
#include <sys/kernel.h>

#include <machine/reg.h>

#define	TM_CTRL		(*(volatile uint32_t *)0x60000040)
#define	TM_PS		(*(volatile uint32_t *)0x60000044)
#define	TM_CNTDIAG	(*(volatile uint32_t *)0x6000004c)	/* timer 1 count */
#define	TM_TIMER1	(*(volatile uint32_t *)0x60000054)
#define	TM_CTRL_STP	0x00001000
#define	TM_CTRL_ENABLE	(0x20 | 0x200 | 0x2 | 0x2000) /* AUTO LOAD INTEN SEL */

/* Measured on the target: the prescaler gives a 50 kHz count. */
#define	TM_PRESCALE	24
#define	TM_RATE		50000

#define	TM_USEC_PER_COUNT	(1000000 / TM_RATE)

static int
clockintr(void *arg)
{
	struct trapframe *tf = arg;
	struct clockframe cf = { .pc = tf->tf_pc, .psr = tf->tf_psr };

	hardclock(&cf);
	return 1;
}

void
cpu_initclocks(void)
{

	TM_CTRL = TM_CTRL_STP;
	TM_PS = TM_PRESCALE;
	TM_TIMER1 = TM_RATE / hz - 1;
	padre_intr_establish(IRQ_TIMER1, IPL_SCHED, clockintr, NULL);
	TM_CTRL = TM_CTRL_ENABLE;
}

void
setstatclockrate(int newhz)
{
}

/*
 * Counts down timer 1, which Linux leaves running at the same rate, as its
 * do_gettimeoffset() does. The count reloads at zero, once per tick.
 */
void
delay(unsigned int us)
{
	const uint32_t period = TM_RATE / hz;
	uint32_t last = TM_CNTDIAG, now, left;

	left = (us + TM_USEC_PER_COUNT - 1) / TM_USEC_PER_COUNT + 1;
	while (left > 0) {
		now = TM_CNTDIAG;
		if (now == last)
			continue;
		left -= MIN(left, (last + period - now) % period);
		last = now;
	}
}
