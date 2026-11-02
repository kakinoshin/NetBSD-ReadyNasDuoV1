/*	$NetBSD$	*/

/*
 * PSR.PIL on the IT3107 is one global-disable bit, not a priority level, so
 * spl is implemented with the INTC masks. See padre/intr.c.
 *
 * Defining PSR_IMPL here turns <sparc/psl.h> into a no-op, which keeps its
 * PIL-based spl functions out of the kernel.
 */
#ifndef PSR_IMPL

#define	PSR_IMPL	0xf0000000	/* implementation */
#define	PSR_VER		0x0f000000	/* version */
#define	PSR_ICC		0x00f00000	/* integer condition codes */
#define	PSR_N		0x00800000	/* negative */
#define	PSR_Z		0x00400000	/* zero */
#define	PSR_O		0x00200000	/* overflow */
#define	PSR_C		0x00100000	/* carry */
#define	PSR_EC		0x00002000	/* coprocessor enable */
#define	PSR_EF		0x00001000	/* FP enable */
#define	PSR_PIL		0x00000f00	/* global interrupt disable */
#define	PSR_S		0x00000080	/* supervisor (kernel) mode */
#define	PSR_PS		0x00000040	/* previous supervisor mode (traps) */
#define	PSR_ET		0x00000020	/* trap enable */
#define	PSR_CWP		0x0000001f	/* current window pointer */

#define	PSR_BITS "\20\16EC\15EF\10S\7PS\6ET"

#define	IPL_NONE	0
#define	IPL_SOFTCLOCK	1
#define	IPL_SOFTBIO	2
#define	IPL_SOFTNET	3
#define	IPL_SOFTSERIAL	4
#define	IPL_VM		5
#define	IPL_SCHED	6
#define	IPL_HIGH	7
#define	NIPL		8

#if (defined(_KERNEL) || defined(_KMEMUSER)) && !defined(_LOCORE)
typedef uint8_t ipl_t;
typedef struct {
	ipl_t _ipl;
} ipl_cookie_t;
#endif

#if defined(_KERNEL) && !defined(_LOCORE)

static __inline __attribute__((__always_inline__)) int
getpsr(void)
{
	int psr;

	__asm volatile("rd %%psr,%0" : "=r" (psr));
	return psr;
}

static __inline __attribute__((__always_inline__)) void
setpsr(int newpsr)
{
	__asm volatile("wr %0,0,%%psr" : : "r" (newpsr) : "memory");
	__asm volatile("nop; nop; nop");
}

static __inline ipl_cookie_t
makeiplcookie(ipl_t ipl)
{

	return (ipl_cookie_t){._ipl = ipl};
}

int	splraiseipl(ipl_cookie_t);
void	splx(int);

#define	spl0()	splx(IPL_NONE)

#include <sys/spl.h>

#endif /* _KERNEL && !_LOCORE */

#endif /* PSR_IMPL */
