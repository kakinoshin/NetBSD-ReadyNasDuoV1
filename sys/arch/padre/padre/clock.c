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
#define	TM_TIMER1	(*(volatile uint32_t *)0x60000054)
#define	TM_CTRL_STP	0x00001000
#define	TM_CTRL_ENABLE	(0x20 | 0x200 | 0x2 | 0x2000) /* AUTO LOAD INTEN SEL */

/* Measured on the target: the prescaler gives a 50 kHz count. */
#define	TM_PRESCALE	24
#define	TM_RATE		50000

/* Rough until there is a free-running counter to calibrate against. */
#define	DELAY_LOOPS_US	100

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

void
delay(unsigned int us)
{
	volatile unsigned int n = us * DELAY_LOOPS_US;

	while (n != 0)
		n--;
}
