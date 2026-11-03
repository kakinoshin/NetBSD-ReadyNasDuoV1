/*	$NetBSD$	*/

#ifndef _PADRE_PCI_MACHDEP_H_
#define _PADRE_PCI_MACHDEP_H_

typedef void		*pci_chipset_tag_t;
typedef u_int		pcitag_t;
typedef int		pci_intr_handle_t;

struct pci_attach_args;
struct pcibus_attach_args;

void		pci_attach_hook(device_t, device_t,
		    struct pcibus_attach_args *);
int		pci_bus_maxdevs(pci_chipset_tag_t, int);
pcitag_t	pci_make_tag(pci_chipset_tag_t, int, int, int);
void		pci_decompose_tag(pci_chipset_tag_t, pcitag_t, int *, int *,
		    int *);
pcireg_t	pci_conf_read(pci_chipset_tag_t, pcitag_t, int);
void		pci_conf_write(pci_chipset_tag_t, pcitag_t, int, pcireg_t);
int		pci_intr_map(const struct pci_attach_args *,
		    pci_intr_handle_t *);
const char	*pci_intr_string(pci_chipset_tag_t, pci_intr_handle_t, char *,
		    size_t);
const struct evcnt *pci_intr_evcnt(pci_chipset_tag_t, pci_intr_handle_t);
void		*pci_intr_establish(pci_chipset_tag_t, pci_intr_handle_t, int,
		    int (*)(void *), void *);
void		pci_intr_disestablish(pci_chipset_tag_t, void *);

#endif /* _PADRE_PCI_MACHDEP_H_ */
