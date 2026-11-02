/*	$NetBSD$	*/

#ifndef _PADRE_INTR_H_
#define _PADRE_INTR_H_

#include <machine/psl.h>

#define	IPL_SAFEPRI	0

#define	NIRQ		48
#define	IRQ_UART0	8
#define	IRQ_TIMER1	46

#if defined(_KERNEL) && !defined(_LOCORE)
struct trapframe;

void	padre_intr_init(void);
void	*padre_intr_establish(int, int, int (*)(void *), void *);
void	padre_intr_dispatch(struct trapframe *);
#endif

#endif /* _PADRE_INTR_H_ */
