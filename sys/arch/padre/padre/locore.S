/*	$NetBSD$	*/

/*
 * Entry and trap table for NetBSD/padre. padre_go.ko enters the first byte of
 * the image with traps off and the MMU's kernel direct map active.
 *
 * Only supervisor mode exists so far. Window traps, interrupts and
 * data misses in the translated kernel window are handled;
 * every other trap prints
 * "TRAP <tbr> <pc> <npc> <dfsr> <ifsr> <dfaddr>" by polling, then stops. The
 * interrupted stack is used for interrupts because no user mode can be
 * interrupted yet.
 */

#include "assym.h"

#include <machine/param.h>
#include <machine/psl.h>
#include <machine/pmap.h>

#define	PSR_BOOT	(PSR_PIL | PSR_S | PSR_PS)
#define	UART0		0x60000060
#define	ASI_MMU		0x10
#define	ASI_TLBPROBE	0x11
#define	MMU_DFSR	0x10
#define	MMU_IFSR	0x14
#define	MMU_DFADDR	0x18
#define	UART_STAT_TH	0x4
#define	NWINDOWS	8
#define	WIM_MASK	((1 << NWINDOWS) - 1)
#define	CCFSZ		96
#define	PAUSE		nop; nop; nop

#define	TRAP(h)		sethi %hi(h), %l0; jmp %l0 + %lo(h); nop; nop
#define	BAD		TRAP(bad_trap)
#define	DFAULT		rd %psr, %l0; sethi %hi(data_fault), %l3; \
			jmp %l3 + %lo(data_fault); nop
#define	IRQ		rd %psr, %l0; sethi %hi(irq_entry), %l3; \
			jmp %l3 + %lo(irq_entry); nop

/* Traps are off here, so this uses no stack. */
#define	TX_G1				\
9:	ld	[%l3 + 4], %l0;		\
	andcc	%l0, UART_STAT_TH, %g0;	\
	be	9b;			\
	 nop;				\
	st	%g1, [%l3]

#define	TX(c)	mov c, %g1; TX_G1

	.text
	.globl	_start, start
_start:
start:
trapbase:
	b	entry			/* tt 0x00: entry from the loader */
	 nop
	nop
	nop
	.rept	4			/* tt 0x01-0x04 */
	BAD
	.endr
	TRAP(window_overflow)		/* tt 0x05 */
	TRAP(window_underflow)		/* tt 0x06 */
	.rept	10			/* tt 0x07-0x10 */
	BAD
	.endr
	.rept	15			/* tt 0x11-0x1f: interrupt levels 1-15 */
	IRQ
	.endr
	.rept	12			/* tt 0x20-0x2b */
	BAD
	.endr
	DFAULT				/* tt 0x2c: data MMU miss */
	.rept	211			/* tt 0x2d-0xff */
	BAD
	.endr

entry:
	set	UART0, %l3		/* first sign of life, before anything else */
	TX('N')
	set	PSR_BOOT, %g1
	wr	%g1, 0, %psr
	PAUSE
	mov	2, %g2			/* window 1 is invalid */
	wr	%g2, 0, %wim
	PAUSE
	set	trapbase, %g2
	wr	%g2, 0, %tbr
	PAUSE

	set	__bss_start, %g2
	set	_end, %g3
1:	cmp	%g2, %g3
	bgeu	2f
	 nop
	st	%g0, [%g2]
	b	1b
	 add	%g2, 4, %g2
2:
	set	boot_stack + USPACE - CCFSZ, %sp
	mov	%g0, %fp
	wr	%g1, PSR_ET, %psr
	PAUSE
	call	padre_bootstrap
	 nop
	call	main
	 nop
halt:
	b	halt
	 nop

/*
 * Save the window after this one, which is the oldest, and make it the
 * invalid one. Preserves %l0-%l2. Clobbers %l3, %l4 and %l7.
 */
#define	SPILL_NEXT			\
	rd	%wim, %l3;		\
	mov	%g1, %l7;		\
	srl	%l3, 1, %g1;		\
	sll	%l3, NWINDOWS - 1, %l4;	\
	or	%g1, %l4, %g1;		\
	and	%g1, WIM_MASK, %g1;	\
	save;				\
	wr	%g1, 0, %wim;		\
	PAUSE;				\
	std	%l0, [%sp + 0];		\
	std	%l2, [%sp + 8];		\
	std	%l4, [%sp + 16];	\
	std	%l6, [%sp + 24];	\
	std	%i0, [%sp + 32];	\
	std	%i2, [%sp + 40];	\
	std	%i4, [%sp + 48];	\
	std	%i6, [%sp + 56];	\
	restore;			\
	mov	%l7, %g1

#define	LOAD_WINDOW			\
	ldd	[%sp + 0], %l0;		\
	ldd	[%sp + 8], %l2;		\
	ldd	[%sp + 16], %l4;	\
	ldd	[%sp + 24], %l6;	\
	ldd	[%sp + 32], %i0;	\
	ldd	[%sp + 40], %i2;	\
	ldd	[%sp + 48], %i4;	\
	ldd	[%sp + 56], %i6

/* Runs in the invalid window. */
window_overflow:
	SPILL_NEXT
	jmp	%l1
	 rett	%l2

/* Rotates %wim left and reloads the window being returned to. */
window_underflow:
	rd	%wim, %l3
	sll	%l3, 1, %l4
	srl	%l3, NWINDOWS - 1, %l5
	or	%l5, %l4, %l5
	and	%l5, WIM_MASK, %l5
	wr	%l5, 0, %wim
	PAUSE
	restore
	restore
	LOAD_WINDOW
	save
	save
	jmp	%l1
	 rett	%l2

/*
 * Data MMU miss. Load a valid PTE of the translated kernel window into the TLB
 * and retry. A second miss on the same address means the load did not help,
 * and goes to bad_trap rather than looping. DFSR is read last because the read
 * clears it and bad_trap reports it.
 *
 * Traps are off here and a trap would reset, so nothing that runs with traps
 * off, such as the window handlers, may touch this window.
 */
data_fault:
	mov	MMU_DFADDR, %l4
	lda	[%l4] ASI_MMU, %l6
	sethi	%hi(KVA_START), %l4
	cmp	%l6, %l4
	blu	bad_trap
	 nop
	srl	%l6, PGSHIFT, %l6
	sll	%l6, PGSHIFT, %l6
	sub	%l6, %l4, %l5
	srl	%l5, PGSHIFT - 2, %l5
	set	PMAP_PT_VA, %l4
	ld	[%l4 + %l5], %l5
	btst	1, %l5
	be	bad_trap
	 nop
	set	pmap_tlb_lastva, %l4
	ld	[%l4], %l7
	cmp	%l7, %l6
	be	bad_trap
	 nop
	st	%l6, [%l4]
	mov	MMU_DFSR, %l4
	lda	[%l4] ASI_MMU, %g0
	sta	%l5, [%l6] ASI_TLBPROBE
	wr	%l0, 0, %psr
	PAUSE
	jmp	%l1
	 rett	%l2

/*
 * Interrupt entry, in the trap window with the PSR in %l0. The hardware does
 * not check %wim on a trap, so this may be the invalid window. Then the next
 * window is the oldest live one and must go to the stack before C can run.
 *
 * C runs with traps on, for window traps, and PIL set. padre_intr_dispatch()
 * works from the INTC, so the level the trap arrived on is not needed.
 */
irq_entry:
	and	%l0, PSR_CWP, %l4
	rd	%wim, %l3
	srl	%l3, %l4, %l5
	btst	1, %l5
	be	1f
	 nop
	SPILL_NEXT
1:
	sub	%fp, CCFSZ + TF_SIZE, %sp
	st	%l0, [%sp + CCFSZ + TF_PSR]
	st	%l1, [%sp + CCFSZ + TF_PC]
	st	%l2, [%sp + CCFSZ + TF_NPC]
	rd	%y, %l3
	st	%l3, [%sp + CCFSZ + TF_Y]
	st	%g1, [%sp + CCFSZ + TF_GLOBAL + 1*4]
	std	%g2, [%sp + CCFSZ + TF_GLOBAL + 2*4]
	std	%g4, [%sp + CCFSZ + TF_GLOBAL + 4*4]
	std	%g6, [%sp + CCFSZ + TF_GLOBAL + 6*4]
	std	%i0, [%sp + CCFSZ + TF_OUT + 0*4]
	std	%i2, [%sp + CCFSZ + TF_OUT + 2*4]
	std	%i4, [%sp + CCFSZ + TF_OUT + 4*4]
	std	%i6, [%sp + CCFSZ + TF_OUT + 6*4]

	or	%l0, PSR_PIL, %l4
	wr	%l4, PSR_ET, %psr
	PAUSE
	call	padre_intr_dispatch
	 add	%sp, CCFSZ, %o0
	or	%l0, PSR_PIL, %l4
	wr	%l4, 0, %psr
	PAUSE

	ld	[%sp + CCFSZ + TF_Y], %l3
	wr	%l3, 0, %y
	ld	[%sp + CCFSZ + TF_GLOBAL + 1*4], %g1
	ldd	[%sp + CCFSZ + TF_GLOBAL + 2*4], %g2
	ldd	[%sp + CCFSZ + TF_GLOBAL + 4*4], %g4
	ldd	[%sp + CCFSZ + TF_GLOBAL + 6*4], %g6

	/* rett into an invalid window would reset, so reload it first. */
	and	%l0, PSR_CWP, %l4
	add	%l4, 1, %l4
	and	%l4, NWINDOWS - 1, %l4
	rd	%wim, %l3
	srl	%l3, %l4, %l5
	btst	1, %l5
	be	2f
	 nop
	sll	%l3, 1, %l4
	srl	%l3, NWINDOWS - 1, %l5
	or	%l5, %l4, %l5
	and	%l5, WIM_MASK, %l5
	wr	%l5, 0, %wim
	PAUSE
	restore
	LOAD_WINDOW
	save
2:
	wr	%l0, 0, %psr
	PAUSE
	jmp	%l1
	 rett	%l2

bad_trap:
	set	UART0, %l3
	TX('\r')
	TX('\n')
	TX('T')
	TX('R')
	TX('A')
	TX('P')
	TX(' ')
	rd	%tbr, %l5
	call	hex32
	 nop
	TX(' ')
	mov	%l1, %l5
	call	hex32
	 nop
	TX(' ')
	mov	%l2, %l5
	call	hex32
	 nop
	TX(' ')
	mov	MMU_DFSR, %l4
	lda	[%l4] ASI_MMU, %l5
	call	hex32
	 nop
	TX(' ')
	mov	MMU_IFSR, %l4
	lda	[%l4] ASI_MMU, %l5
	call	hex32
	 nop
	TX(' ')
	mov	MMU_DFADDR, %l4
	lda	[%l4] ASI_MMU, %l5
	call	hex32
	 nop
	TX('\r')
	TX('\n')
1:	b	1b
	 nop

/* Print %l5 in hex. %o7 is this window's scratch. */
hex32:
	mov	8, %l6
1:	srl	%l5, 28, %g1
	sll	%l5, 4, %l5
	cmp	%g1, 10
	bl	2f
	 add	%g1, '0', %g1
	add	%g1, 'a' - '0' - 10, %g1
2:	TX_G1
	subcc	%l6, 1, %l6
	bne	1b
	 nop
	retl
	 nop

	.globl	lwp0uarea
	.bss
	.align	8192
boot_stack:
lwp0uarea:
	.skip	USPACE
