/*	$NetBSD$	*/

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/intr.h>

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

	rootconf();
}

void
device_register(device_t dev, void *aux)
{
}
