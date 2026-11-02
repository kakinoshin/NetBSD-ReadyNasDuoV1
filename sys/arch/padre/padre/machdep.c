/*	$NetBSD$	*/

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/cpu.h>
#include <sys/intr.h>
#include <sys/kernel.h>
#include <sys/lwp.h>
#include <sys/proc.h>
#include <sys/reboot.h>

#include <uvm/uvm_extern.h>

#include <dev/cons.h>

extern char end[];
extern char lwp0uarea[];

struct cpu_info cpu_info_store;
struct vm_map *phys_map;

char machine[] = MACHINE;
char machine_arch[] = MACHINE_ARCH;

void	padre_bootstrap(void);

/* Called from locore on the boot stack, with bss clear and traps on. */
void
padre_bootstrap(void)
{
	struct cpu_info * const ci = curcpu();

	ci->ci_self = ci;
	ci->ci_curlwp = &lwp0;
	ci->ci_onproc = &lwp0;
	lwp0.l_cpu = ci;

	/* Progress marks: printf is not usable until main() runs consinit(). */
	consinit();
	cn_tab->cn_putc(NODEV, 'B');

	padre_intr_init();
	setpsr(getpsr() & ~PSR_PIL);
	cn_tab->cn_putc(NODEV, 'I');

	pmap_bootstrap((vaddr_t)end);
	cn_tab->cn_putc(NODEV, 'P');
	uvm_lwp_setuarea(&lwp0, (vaddr_t)lwp0uarea);
	cn_tab->cn_putc(NODEV, '\r');
	cn_tab->cn_putc(NODEV, '\n');
}

void
cpu_startup(void)
{

	cpu_setmodel("Infrant IT3107");
	banner();
}

/* There is only one system call path. */
void
syscall_intern(struct proc *p)
{
}

void
cpu_dumpconf(void)
{
}

void
cpu_need_resched(struct cpu_info *ci, struct lwp *l, int flags)
{

	ci->ci_want_resched = 1;
}

void
cpu_signotify(struct lwp *l)
{

	curcpu()->ci_want_ast = 1;
}

void
cpu_idle(void)
{
}

/* There are no faults to recover from until user mode exists. */
int
kcopy(const void *src, void *dst, size_t len)
{

	memcpy(dst, src, len);
	return 0;
}

void
cpu_reboot(int howto, char *bootstr)
{

	printf("halted\n");
	setpsr(getpsr() | PSR_PIL);
	for (;;)
		continue;
}
