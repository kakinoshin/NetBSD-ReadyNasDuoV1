/*	$NetBSD$	*/

/*
 * The IT3107 PCI host bridge. iboot and Linux leave PCI on, with the swap
 * register set as Linux's pcic.c does, and with the BARs assigned, so the bus
 * attaches as it is. Device windows follow Linux: I/O at CPU 0x5c000000 and
 * memory at CPU 0x4c000000 plus the bus address.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/cpu.h>
#include <sys/device.h>
#include <sys/intr.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#define	PCI_CONFIG_ADDRESS	(*(volatile uint32_t *)0x50000cf8)
#define	PCI_CONFIG_DATA		(*(volatile uint32_t *)0x50000cfc)
#define	PCI_CONFIG_EN		0x80000000

/*
 * Linux's pcibios_fixup_irqs() picks the line of the USB chip's pin B by
 * board. On the Duo all three USB functions share 27, as RAIDiator's
 * /proc/interrupts shows; line 26 stays asserted, with nothing to clear it.
 */
#define	IRQ_PCI_INTA		27

static const struct padre_bus_space pcic_io = { .pbs_base = 0x5c000000 };
static const struct padre_bus_space pcic_mem = { .pbs_base = 0x4c000000 };

static int
pcic_match(device_t parent, cfdata_t cf, void *aux)
{

	return strcmp(aux, "pcic") == 0;
}

static void
pcic_attach(device_t parent, device_t self, void *aux)
{
	struct pcibus_attach_args pba;

	aprint_normal("\n");
	memset(&pba, 0, sizeof(pba));
	pba.pba_iot = &pcic_io;
	pba.pba_memt = &pcic_mem;
	pba.pba_dmat = &padre_bus_dma_tag;
	pba.pba_pc = NULL;
	pba.pba_bus = 0;
	pba.pba_flags = PCI_FLAGS_IO_OKAY | PCI_FLAGS_MEM_OKAY;
	config_found(self, &pba, pcibusprint, CFARGS(.iattr = "pcibus"));
}

CFATTACH_DECL_NEW(pcic, 0, pcic_match, pcic_attach, NULL, NULL);

void
pci_attach_hook(device_t parent, device_t self,
    struct pcibus_attach_args *pba)
{
}

int
pci_bus_maxdevs(pci_chipset_tag_t pc, int bus)
{

	return 32;
}

pcitag_t
pci_make_tag(pci_chipset_tag_t pc, int bus, int dev, int fn)
{

	return (bus << 16) | (dev << 11) | (fn << 8);
}

void
pci_decompose_tag(pci_chipset_tag_t pc, pcitag_t tag, int *bp, int *dp,
    int *fp)
{

	if (bp != NULL)
		*bp = (tag >> 16) & 0xff;
	if (dp != NULL)
		*dp = (tag >> 11) & 0x1f;
	if (fp != NULL)
		*fp = (tag >> 8) & 0x7;
}

pcireg_t
pci_conf_read(pci_chipset_tag_t pc, pcitag_t tag, int reg)
{
	const u_int psr = getpsr();
	pcireg_t v;

	if ((unsigned)reg >= PCI_CONF_SIZE || (tag >> 16) != 0)
		return 0xffffffff;
	setpsr(psr | PSR_PIL);
	PCI_CONFIG_ADDRESS = PCI_CONFIG_EN | tag | reg;
	v = PCI_CONFIG_DATA;
	PCI_CONFIG_ADDRESS = 0;
	setpsr(psr);
	return v;
}

void
pci_conf_write(pci_chipset_tag_t pc, pcitag_t tag, int reg, pcireg_t v)
{
	const u_int psr = getpsr();

	if ((unsigned)reg >= PCI_CONF_SIZE || (tag >> 16) != 0)
		return;
	setpsr(psr | PSR_PIL);
	PCI_CONFIG_ADDRESS = PCI_CONFIG_EN | tag | reg;
	PCI_CONFIG_DATA = v;
	PCI_CONFIG_ADDRESS = 0;
	setpsr(psr);
}

int
pci_intr_map(const struct pci_attach_args *pa, pci_intr_handle_t *ihp)
{

	if (pa->pa_intrpin == 0)
		return 1;
	*ihp = IRQ_PCI_INTA;
	return 0;
}

const char *
pci_intr_string(pci_chipset_tag_t pc, pci_intr_handle_t ih, char *buf,
    size_t len)
{

	snprintf(buf, len, "irq %d", ih);
	return buf;
}

const struct evcnt *
pci_intr_evcnt(pci_chipset_tag_t pc, pci_intr_handle_t ih)
{

	return NULL;
}

void *
pci_intr_establish(pci_chipset_tag_t pc, pci_intr_handle_t ih, int ipl,
    int (*fun)(void *), void *arg)
{

	return padre_intr_establish(ih, ipl, fun, arg);
}

int
pci_intr_setattr(pci_chipset_tag_t pc, pci_intr_handle_t *ihp, int attr,
    uint64_t data)
{

	return ENODEV;
}

void
pci_intr_disestablish(pci_chipset_tag_t pc, void *cookie)
{

	panic("%s: not implemented", __func__);
}
