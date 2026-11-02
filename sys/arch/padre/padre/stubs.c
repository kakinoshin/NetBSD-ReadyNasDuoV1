/*	$NetBSD$	*/

/*
 * Machine-dependent entry points that bring-up has not reached yet. Each one
 * stops the kernel by name, so the first missing piece identifies itself.
 *
 * This file includes no kernel headers on purpose: the stubs only need a
 * symbol, so they do not have to match the real prototypes.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

void	panic(const char *, ...) __dead;

#define	STUB(name)							\
void name(void);							\
void									\
name(void)								\
{									\
	panic("%s: not implemented", #name);				\
}

STUB(_ufetch_8)
STUB(_ufetch_16)
STUB(_ufetch_32)
STUB(_ustore_8)
STUB(_ustore_16)
STUB(_ustore_32)
STUB(copyin)
STUB(copyinstr)
STUB(copyout)
STUB(copyoutstr)
STUB(cpu_coredump)
STUB(cpu_getmcontext)
STUB(cpu_lwp_fork)
STUB(cpu_lwp_free)
STUB(cpu_lwp_free2)
STUB(cpu_lwp_setprivate)
STUB(cpu_mcontext_validate)
STUB(cpu_setmcontext)
STUB(cpu_spawn_return)
STUB(cpu_switchto)
STUB(md_child_return)
STUB(mm_md_physacc)
STUB(mm_md_readwrite)
STUB(process_read_fpregs)
STUB(process_read_regs)
STUB(process_set_pc)
STUB(process_write_fpregs)
STUB(process_write_regs)
STUB(sendsig_siginfo)
STUB(setregs)
STUB(startlwp)
STUB(sys_sysarch)
STUB(vmapbuf)
STUB(vunmapbuf)
