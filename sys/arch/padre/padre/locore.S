/*	$NetBSD$	*/

/*
 * Entry and trap table for NetBSD/padre. padre_go.ko enters the first byte of
 * the image with traps off and the MMU's kernel direct map active.
 *
 * A trap from supervisor mode runs on the interrupted stack. Only interrupts
 * and MMU faults are expected there: anything else prints
 * "TRAP <tbr> <pc> <npc> <dfsr> <ifsr> <dfaddr>" by polling, then stops.
 *
 * A trap from user mode moves every user window to pcb_rw, newest first, and
 * gives the kernel the whole register file, so no window handler ever touches
 * user memory with traps off. windows_prepare() writes the older windows to the
 * user stack, which can fault normally, before the return to user mode.
 */

#include "assym.h"

#include <machine/param.h>
#include <machine/psl.h>

#define	PSR_BOOT	(PSR_PIL | PSR_S | PSR_PS)
#define	USER_TOP	0x40000000
#define	TT_TEXTFAULT	0x01
#define	TT_DATAFAULT	0x09
#define	TT_IRQ_FIRST	0x11
#define	TT_IRQ_LAST	0x1f
#define	TT_DATAMISS	0x2c
#define	TT_TEXTMISS	0x3c
#define	UART0		0x60000060
#define	ASI_MMU		0x10
#define	MMU_DFSR	0x10
#define	MMU_IFSR	0x14
#define	MMU_DFADDR	0x18
#define	UART_STAT_TH	0x4
#define	NWINDOWS	8
#define	WIM_MASK	((1 << NWINDOWS) - 1)
#define	CCFSZ		96
#define	CURLWP		(cpu_info_store + CI_CURLWP)
#define	PAUSE		nop; nop; nop

#define	TRAP(h)		sethi %hi(h), %l0; jmp %l0 + %lo(h); nop; nop
#define	VEC(h)		rd %psr, %l0; sethi %hi(h), %l3; jmp %l3 + %lo(h); nop
#define	COMMON		VEC(trap_common)

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
	COMMON
	.endr
	VEC(window_overflow)		/* tt 0x05 */
	VEC(window_underflow)		/* tt 0x06 */
	.rept	249			/* tt 0x07-0xff */
	COMMON
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
	btst	PSR_PS, %l0
	be	trap_common
	 nop
	SPILL_NEXT
	wr	%l0, 0, %psr
	PAUSE
	jmp	%l1
	 rett	%l2

/* Rotates %wim left and reloads the window being returned to. */
window_underflow:
	btst	PSR_PS, %l0
	be	trap_common
	 nop
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
	wr	%l0, 0, %psr
	PAUSE
	jmp	%l1
	 rett	%l2

/* In the trap window: psr in %l0, pc in %l1, npc in %l2. */
trap_common:
	btst	PSR_PS, %l0
	bne	kernel_trap
	 nop

	/* Registers first, which frees the globals for the window loop. */
	sethi	%hi(CURLWP), %l3
	ld	[%l3 + %lo(CURLWP)], %l3
	ld	[%l3 + L_PCB], %l4
	set	USPACE - TF_SIZE, %l5
	add	%l4, %l5, %l5
	st	%l0, [%l5 + TF_PSR]
	st	%l1, [%l5 + TF_PC]
	st	%l2, [%l5 + TF_NPC]
	rd	%y, %l3
	st	%l3, [%l5 + TF_Y]
	st	%g1, [%l5 + TF_GLOBAL + 1*4]
	std	%g2, [%l5 + TF_GLOBAL + 2*4]
	std	%g4, [%l5 + TF_GLOBAL + 4*4]
	std	%g6, [%l5 + TF_GLOBAL + 6*4]
	std	%i0, [%l5 + TF_OUT + 0*4]
	std	%i2, [%l5 + TF_OUT + 2*4]
	std	%i4, [%l5 + TF_OUT + 4*4]
	std	%i6, [%l5 + TF_OUT + 6*4]

	add	%l4, PCB_RW, %g1
	rd	%wim, %g2
	clr	%g3
	and	%l0, PSR_CWP, %g4
	mov	%l0, %g5
1:	add	%g4, 1, %g4
	and	%g4, NWINDOWS - 1, %g4
	srl	%g2, %g4, %g6
	btst	1, %g6
	bne	2f
	 nop
	restore
	std	%l0, [%g1 + 0]
	std	%l2, [%g1 + 8]
	std	%l4, [%g1 + 16]
	std	%l6, [%g1 + 24]
	std	%i0, [%g1 + 32]
	std	%i2, [%g1 + 40]
	std	%i4, [%g1 + 48]
	std	%i6, [%g1 + 56]
	add	%g1, 64, %g1
	b	1b
	 inc	%g3
2:	wr	%g5, 0, %psr
	PAUSE
	st	%g3, [%l4 + PCB_NSAVED]

	/*
	 * Not before the loop: when this window was the invalid one, its %sp
	 * is the %fp of the oldest user window.
	 */
	sub	%l5, CCFSZ, %sp

	/* The kernel owns all windows down from here to the user's. */
	and	%l0, PSR_CWP, %g4
	add	%g4, 1, %g4
	and	%g4, NWINDOWS - 1, %g4
	mov	1, %g2
	sll	%g2, %g4, %g2
	wr	%g2, 0, %wim
	PAUSE

	rd	%tbr, %o0
	srl	%o0, 4, %o0
	and	%o0, 0xff, %o0
	mov	%l5, %o1
	cmp	%o0, TT_IRQ_FIRST
	blu	3f
	 cmp	%o0, TT_IRQ_LAST
	bgu	3f
	 or	%l0, PSR_PIL, %l4
	wr	%l4, PSR_ET, %psr
	PAUSE
	call	padre_intr_dispatch
	 mov	%o1, %o0
	wr	%l0, PSR_ET, %psr
	PAUSE
	call	trap_userret
	 add	%sp, CCFSZ, %o0
	b	user_return
	 nop
3:
	/* PIL stays up until trap() has read the fault registers. */
	or	%l0, PSR_PIL, %l4
	wr	%l4, PSR_ET, %psr
	PAUSE
	call	trap
	 nop

/* Also the way out of lwp_trampoline. The trapframe is at %sp + CCFSZ. */
	.globl	user_return
user_return:
	call	windows_prepare
	 add	%sp, CCFSZ, %o0

	/* A switch may have moved this frame to any window: read the CWP. */
	rd	%psr, %l6
	andn	%l6, PSR_ET, %l6
	wr	%l6, 0, %psr
	PAUSE
	add	%sp, CCFSZ, %l5
	sethi	%hi(CURLWP), %l3
	ld	[%l3 + %lo(CURLWP)], %l3
	ld	[%l3 + L_PCB], %l4
	ld	[%l4 + PCB_NSAVED], %g3
	add	%l4, PCB_RW, %g1
	and	%l6, PSR_CWP, %g4
	add	%g4, %g3, %g4
	add	%g4, 1, %g4
	and	%g4, NWINDOWS - 1, %g4
	mov	%l6, %g5
	wr	%g0, 0, %wim
	PAUSE
1:	restore
	ldd	[%g1 + 0], %l0
	ldd	[%g1 + 8], %l2
	ldd	[%g1 + 16], %l4
	ldd	[%g1 + 24], %l6
	ldd	[%g1 + 32], %i0
	ldd	[%g1 + 40], %i2
	ldd	[%g1 + 48], %i4
	ldd	[%g1 + 56], %i6
	deccc	%g3
	bne	1b
	 add	%g1, 64, %g1
	wr	%g5, 0, %psr
	PAUSE
	mov	1, %g2
	sll	%g2, %g4, %g2
	wr	%g2, 0, %wim
	PAUSE

	ld	[%l5 + TF_PSR], %l0
	ld	[%l5 + TF_PC], %l1
	ld	[%l5 + TF_NPC], %l2
	ld	[%l5 + TF_Y], %l3
	wr	%l3, 0, %y
	ld	[%l5 + TF_GLOBAL + 1*4], %g1
	ldd	[%l5 + TF_GLOBAL + 2*4], %g2
	ldd	[%l5 + TF_GLOBAL + 4*4], %g4
	ldd	[%l5 + TF_GLOBAL + 6*4], %g6
	ldd	[%l5 + TF_OUT + 0*4], %i0
	ldd	[%l5 + TF_OUT + 2*4], %i2
	ldd	[%l5 + TF_OUT + 4*4], %i4
	ldd	[%l5 + TF_OUT + 6*4], %i6
	set	PSR_ICC, %l3
	and	%l0, %l3, %l0
	and	%l6, PSR_CWP, %l3
	or	%l0, %l3, %l0
	or	%l0, PSR_S, %l0
	wr	%l0, 0, %psr
	PAUSE
	jmp	%l1
	 rett	%l2

/*
 * A trap from the kernel, in the trap window. The hardware does not check %wim
 * on a trap, so this may be the invalid window. Then the next window is the
 * oldest live one and must go to the stack before C can run.
 *
 * C runs with traps on, for window traps, and PIL set. padre_intr_dispatch()
 * works from the INTC, so the level the trap arrived on is not needed.
 */
kernel_trap:
	rd	%tbr, %l6
	srl	%l6, 4, %l6
	and	%l6, 0xff, %l6
	cmp	%l6, TT_DATAMISS
	be	1f
	 cmp	%l6, TT_DATAFAULT
	be	1f
	 cmp	%l6, TT_TEXTMISS
	be	1f
	 cmp	%l6, TT_TEXTFAULT
	be	1f
	 cmp	%l6, TT_IRQ_FIRST
	blu	bad_trap
	 cmp	%l6, TT_IRQ_LAST
	bgu	bad_trap
	 nop
1:
	and	%l0, PSR_CWP, %l4
	rd	%wim, %l3
	srl	%l3, %l4, %l5
	btst	1, %l5
	be	2f
	 nop
	SPILL_NEXT
2:
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
	cmp	%l6, TT_IRQ_FIRST
	blu	3f
	 cmp	%l6, TT_IRQ_LAST
	bgu	3f
	 nop
	call	padre_intr_dispatch
	 add	%sp, CCFSZ, %o0
	b	4f
	 nop
3:	mov	%l6, %o0
	call	trap
	 add	%sp, CCFSZ, %o1
4:
	/* A fault can sleep, and the thread can come back in another window. */
	rd	%psr, %l6
	or	%l6, PSR_PIL, %l4
	wr	%l4, 0, %psr
	PAUSE
	andn	%l4, PSR_ET, %l4
	wr	%l4, 0, %psr
	PAUSE

	ld	[%sp + CCFSZ + TF_PC], %l1
	ld	[%sp + CCFSZ + TF_NPC], %l2
	ld	[%sp + CCFSZ + TF_Y], %l3
	wr	%l3, 0, %y
	ld	[%sp + CCFSZ + TF_GLOBAL + 1*4], %g1
	ldd	[%sp + CCFSZ + TF_GLOBAL + 2*4], %g2
	ldd	[%sp + CCFSZ + TF_GLOBAL + 4*4], %g4
	ldd	[%sp + CCFSZ + TF_GLOBAL + 6*4], %g6

	/* rett into an invalid window would reset, so reload it first. */
	and	%l6, PSR_CWP, %l4
	add	%l4, 1, %l4
	and	%l4, NWINDOWS - 1, %l4
	rd	%wim, %l3
	srl	%l3, %l4, %l5
	btst	1, %l5
	be	5f
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
5:
	andn	%l0, PSR_CWP, %l0
	and	%l6, PSR_CWP, %l4
	or	%l0, %l4, %l0
	wr	%l0, 0, %psr
	PAUSE
	jmp	%l1
	 rett	%l2

/*
 * User access. Each routine is a leaf, so a fault leaves the caller's window
 * and %o3 intact, and trap() resumes at the address in pcb_onfault.
 */
#define	ONFAULT(label)							\
	sethi	%hi(CURLWP), %o3;					\
	ld	[%o3 + %lo(CURLWP)], %o3;				\
	ld	[%o3 + L_PCB], %o3;					\
	set	label, %o4;						\
	st	%o4, [%o3 + PCB_ONFAULT]

/* Fails the routine unless [reg, reg + size) is below the kernel. */
#define	CHECKUSER(reg, size)						\
	sethi	%hi(USER_TOP), %o4;				\
	addcc	reg, size, %o5;						\
	bcs	user_range;						\
	 cmp	%o5, %o4;						\
	bgu	user_range;						\
	 nop

user_range:
	retl
	 mov	EFAULT, %o0

user_fault:
	st	%g0, [%o3 + PCB_ONFAULT]
	retl
	 mov	EFAULT, %o0

	.globl	copyin, copyout
copyin:
	CHECKUSER(%o0, %o2)
	b	copy_common
	 nop
copyout:
	CHECKUSER(%o1, %o2)
copy_common:
	ONFAULT(user_fault)
	or	%o0, %o1, %o4
	btst	3, %o4
	bne	2f
	 nop
1:	cmp	%o2, 4
	blu	2f
	 nop
	ld	[%o0], %o4
	st	%o4, [%o1]
	add	%o0, 4, %o0
	add	%o1, 4, %o1
	b	1b
	 sub	%o2, 4, %o2
2:	tst	%o2
	be	3f
	 nop
	ldub	[%o0], %o4
	stb	%o4, [%o1]
	inc	%o0
	inc	%o1
	b	2b
	 dec	%o2
3:	st	%g0, [%o3 + PCB_ONFAULT]
	retl
	 clr	%o0

/* int copyinstr(from, to, max, done), and copyoutstr with the check on to. */
	.globl	copyinstr, copyoutstr
copyinstr:
	mov	%o0, %g1
	b	copystr_user
	 nop
copyoutstr:
	mov	%o1, %g1
copystr_user:
	mov	%o3, %g4
	ONFAULT(user_fault)
	mov	%o2, %o5
	sethi	%hi(USER_TOP), %g2
1:	tst	%o5
	be	3f
	 mov	ENAMETOOLONG, %g3
	cmp	%g1, %g2
	bgeu	3f
	 mov	EFAULT, %g3
	ldub	[%o0], %o4
	stb	%o4, [%o1]
	inc	%o0
	inc	%o1
	inc	%g1
	tst	%o4
	bne	1b
	 dec	%o5
	clr	%g3
3:	st	%g0, [%o3 + PCB_ONFAULT]
	tst	%g4
	be	4f
	 sub	%o2, %o5, %o2
	st	%o2, [%g4]
4:	retl
	 mov	%g3, %o0

#define	UFETCH(name, size, load, store)					\
	.globl	name;							\
name:									\
	CHECKUSER(%o0, size);						\
	ONFAULT(user_fault);						\
	load	[%o0], %o4;						\
	store	%o4, [%o1];						\
	st	%g0, [%o3 + PCB_ONFAULT];				\
	retl;								\
	 clr	%o0

#define	USTORE(name, size, store)					\
	.globl	name;							\
name:									\
	CHECKUSER(%o0, size);						\
	ONFAULT(user_fault);						\
	store	%o1, [%o0];						\
	st	%g0, [%o3 + PCB_ONFAULT];				\
	retl;								\
	 clr	%o0

	UFETCH(_ufetch_8, 1, ldub, stb)
	UFETCH(_ufetch_16, 2, lduh, sth)
	UFETCH(_ufetch_32, 4, ld, st)
	USTORE(_ustore_8, 1, stb)
	USTORE(_ustore_16, 2, sth)
	USTORE(_ustore_32, 4, st)

/*
 * lwp_t *cpu_switchto(lwp_t *old, lwp_t *new, bool returning)
 *
 * NWINDOWS - 2 saves push every older window to the stack and leave the window
 * above this one invalid, whichever window was invalid before. This window then
 * takes the identity of the new thread's cpu_switchto, or of the frame built by
 * cpu_lwp_fork(), and the restore reloads its caller from the stack through the
 * underflow trap. Nothing here depends on the CWP the thread last ran in.
 *
 * Only %sp and the return address of the caller have to survive: the rest of
 * the out registers, the globals and %y are call-clobbered. Software spl stays
 * as it is because mi_switch() runs at IPL_SCHED on both sides.
 */
	.globl	cpu_switchto
cpu_switchto:
	save	%sp, -CCFSZ, %sp
	rd	%psr, %l0
	or	%l0, PSR_PIL, %l1
	wr	%l1, 0, %psr
	PAUSE
	.rept	NWINDOWS - 2
	save	%sp, -CCFSZ, %sp
	.endr
	.rept	NWINDOWS - 2
	restore
	.endr

	tst	%i0
	be	1f
	 nop
	ld	[%i0 + L_PCB], %l2
	st	%fp, [%l2 + PCB_SP]
	st	%i7, [%l2 + PCB_PC]
1:
	ld	[%i1 + L_PCB], %l2
	ld	[%l2 + PCB_SP], %fp
	ld	[%l2 + PCB_PC], %i7
	sub	%fp, CCFSZ, %sp
	sethi	%hi(CURLWP), %l3
	st	%i1, [%l3 + %lo(CURLWP)]
	wr	%l0, 0, %psr
	PAUSE
	ret
	 restore %i0, 0, %o0

/*
 * First code of a new thread, entered from cpu_switchto() with the old lwp in
 * %o0. cpu_lwp_fork() left the function in %l0 and its argument in %l1.
 */
	.globl	lwp_trampoline
lwp_trampoline:
	sethi	%hi(CURLWP), %o1
	call	lwp_startup
	 ld	[%o1 + %lo(CURLWP)], %o1
	call	%l0
	 mov	%l1, %o0
	b	user_return
	 nop

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

	/*
	 * Bring-up backtrace: "BT <sp> <ret> <i0>" per frame, from the
	 * registers while the next window is valid, then from the stack.
	 */
	mov	16, %g6
2:	TX('B')
	TX('T')
	TX(' ')
	mov	%i6, %l5
	call	hex32
	 nop
	TX(' ')
	mov	%i7, %l5
	call	hex32
	 nop
	TX(' ')
	mov	%i0, %l5
	call	hex32
	 nop
	TX('\r')
	TX('\n')
	subcc	%g6, 1, %g6
	be	9f
	 nop
	rd	%psr, %g2
	and	%g2, PSR_CWP, %g2
	add	%g2, 1, %g2
	and	%g2, NWINDOWS - 1, %g2
	rd	%wim, %g3
	srl	%g3, %g2, %g3
	btst	1, %g3
	bne	3f
	 nop
	restore
	set	UART0, %l3
	b	2b
	 nop

	/* The next window is on the stack; follow it while in the direct map. */
3:	mov	%i6, %g4
4:	set	0x80000000, %g2
	cmp	%g4, %g2
	blu	9f
	 set	0xc0000000, %g2
	cmp	%g4, %g2
	bgeu	9f
	 btst	7, %g4
	bne	9f
	 nop
	TX('b')
	TX('t')
	TX(' ')
	mov	%g4, %l5
	call	hex32
	 nop
	TX(' ')
	ld	[%g4 + 60], %l5
	call	hex32
	 nop
	TX(' ')
	ld	[%g4 + 32], %l5
	call	hex32
	 nop
	TX('\r')
	TX('\n')
	ld	[%g4 + 56], %g4
	subcc	%g6, 1, %g6
	bne	4b
	 nop
9:	b	9b
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
