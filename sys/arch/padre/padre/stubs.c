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

STUB(cpu_coredump)
STUB(mm_md_physacc)
STUB(mm_md_readwrite)
STUB(process_read_fpregs)
STUB(process_read_regs)
STUB(process_set_pc)
STUB(process_write_fpregs)
STUB(process_write_regs)
