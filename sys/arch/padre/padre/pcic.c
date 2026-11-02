/*	$NetBSD$	*/

/*
 * The IT3107 PCI host bridge. iboot and Linux leave PCI on, with the swap
 * register set as Linux's pcic.c does, so config cycles work from the start.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/cpu.h>

#define	PCI_CONFIG_ADDRESS	(*(volatile uint32_t *)0x50000cf8)
#define	PCI_CONFIG_DATA		(*(volatile uint32_t *)0x50000cfc)
#define	PCI_CONFIG_EN		0x80000000

void	padre_pci_scan(void);

static uint32_t
pcic_conf_read(u_int dev, u_int fn, u_int reg)
{
	const u_int psr = getpsr();
	uint32_t v;

	setpsr(psr | PSR_PIL);
	PCI_CONFIG_ADDRESS = PCI_CONFIG_EN | (dev << 11) | (fn << 8) | reg;
	v = PCI_CONFIG_DATA;
	PCI_CONFIG_ADDRESS = 0;
	setpsr(psr);
	return v;
}

/* Bring-up check: read-only, and it touches nothing on the devices. */
void
padre_pci_scan(void)
{
	u_int dev, fn;
	uint32_t id;

	printf("pci: feature %08x swap %08x\n",
	    *(volatile uint32_t *)0x6000010c, *(volatile uint32_t *)0x60000104);
	for (dev = 0; dev < 32; dev++) {
		for (fn = 0; fn < 8; fn++) {
			id = pcic_conf_read(dev, fn, 0x00);
			if (id == 0xffffffff || id == 0)
				continue;
			printf("pci: %u.%u id %08x class %08x bar0 %08x "
			    "bar4 %08x cmd %08x intr %08x\n", dev, fn, id,
			    pcic_conf_read(dev, fn, 0x08),
			    pcic_conf_read(dev, fn, 0x10),
			    pcic_conf_read(dev, fn, 0x20),
			    pcic_conf_read(dev, fn, 0x04),
			    pcic_conf_read(dev, fn, 0x3c));
		}
	}
}
