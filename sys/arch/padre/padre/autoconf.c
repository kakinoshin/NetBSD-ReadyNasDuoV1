/*	$NetBSD$	*/

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/intr.h>
#include <sys/kernel.h>
#include <sys/proc.h>

#include <uvm/uvm_extern.h>

void
cpu_configure(void)
{

	if (config_rootfound("mainbus", NULL) == NULL)
		panic("no mainbus found");
	spl0();
}

void
cpu_rootconf(void)
{
	const int start = getticks();

	/* Bring-up check of the timer, softints and thread switching. */
	kpause("clktest", false, hz, NULL);
	printf("clock: slept %d ticks\n", getticks() - start);
	pmap_selftest();

	rootconf();
}

void
device_register(device_t dev, void *aux)
{
}
