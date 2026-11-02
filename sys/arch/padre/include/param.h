/*	$NetBSD$	*/

/*
 * Machine dependent constants for the IT3107. The userland ABI is NetBSD/sparc.
 */

#define	_MACHINE	padre
#define	MACHINE		"padre"
#define	_MACHINE_ARCH	sparc
#define	MACHINE_ARCH	"sparc"
#define	MID_MACHINE	MID_SPARC

#ifdef _KERNEL
#ifndef _LOCORE
#include <machine/cpu.h>
#endif
#endif

#define	STACK_ALIGNBYTES	(8 - 1)

/*
 * The MMU passes kernel addresses from 0x40000000 up to the vmalloc window
 * straight through to the AHB bus, at AHB address + 0x40000000.
 */
#define	KERNBASE	0x80000000	/* DDR at physical 0x40000000 */
#define	KERNEND		0xffff0000
#define	KVA_START	0xf8000000	/* translated kernel window */

#define	USPACE		8192

#define	MSIZE		256		/* size of an mbuf */

#ifndef MCLSHIFT
#define	MCLSHIFT	11		/* convert bytes to m_buf clusters */
#endif
#define	MCLBYTES	(1 << MCLSHIFT)	/* size of a m_buf cluster */

#define	NKMEMPAGES_MIN_DEFAULT	((16 * 1024 * 1024) >> PAGE_SHIFT)
#define	NKMEMPAGES_MAX_DEFAULT	((64 * 1024 * 1024) >> PAGE_SHIFT)

#if defined(_KERNEL) || defined(_STANDALONE)
#ifndef _LOCORE
#ifndef __HIDE_DELAY
extern void	delay(unsigned int);
#define	DELAY(n)	delay(n)
#endif
#endif

#define	PGSHIFT		12
#define	NBPG		(1 << PGSHIFT)
#define	PGOFSET		(NBPG - 1)
#endif
