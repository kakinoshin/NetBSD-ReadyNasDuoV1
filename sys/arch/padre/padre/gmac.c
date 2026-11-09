/*	$NetBSD$	*/

/*
 * GMAC support needed before any driver attaches.
 *
 * RAIDiator leaves GMAC0 receiving when padre_go.ko takes over, and its DMA
 * then writes LAN frames into Linux's old RX buffers, which are NetBSD's
 * memory by then. gmac_quiesce() stops it the way padre_gmac's close does.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/cpu.h>

#include <padre/padre/gmacreg.h>

#define	GM_REG(o)	(*(volatile uint32_t *)(GM0_BASE + (o)))
#define	M4_REG(o)	(*(volatile uint32_t *)(M4_BASE + (o)))

/* Polls are bounded so that a dead block cannot stop the boot. */
#define	GM_SPIN		100000

/* What RAIDiator left behind, printed once printf works. */
static struct {
	uint32_t	ctrl, cfg1, cfg2, rxctrl, txctrl, rxhead, txhead;
	uint32_t	imask, ffctrl_rx, ffctrl_tx, rxctrl_after;
	int		timeouts;
} gmac_boot;

static void
gmac_wait(void)
{
	int i;

	for (i = 0; i < GM_SPIN; i++)
		if (GM_REG(GM_MACCMD) & GM_MACCMD_DONE)
			return;
	gmac_boot.timeouts++;
}

/* PE-MCXMAC register access for GMAC0, shared with if_gm. */
uint32_t
gmac_macread(int r)
{

	GM_REG(GM_MACCMD) = GM_MACCMD_REG(r) | GM_MACCMD_READ | GM_MACCMD_GO;
	gmac_wait();
	return GM_REG(GM_MACDATA);
}

void
gmac_macwrite(int r, uint32_t v)
{

	GM_REG(GM_MACDATA) = v;
	GM_REG(GM_MACCMD) = GM_MACCMD_REG(r) | GM_MACCMD_GO;
	gmac_wait();
}

/* Called from padre_bootstrap, before NetBSD reuses any memory. */
void
gmac_quiesce(void)
{
	int i;

	gmac_boot.ctrl = GM_REG(GM_CTRL);
	gmac_boot.cfg1 = gmac_macread(MAC_CFG1);
	gmac_boot.cfg2 = gmac_macread(MAC_CFG2);
	gmac_boot.rxctrl = M4_REG(M4_RXCTRL);
	gmac_boot.txctrl = M4_REG(M4_TXCTRL);
	gmac_boot.rxhead = M4_REG(M4_RXHEAD);
	gmac_boot.txhead = M4_REG(M4_TXHEAD);
	gmac_boot.imask = M4_REG(M4_IMASK);
	gmac_boot.ffctrl_rx = *(volatile uint32_t *)MC_DMA_FFCTRL(0);
	gmac_boot.ffctrl_tx = *(volatile uint32_t *)MC_DMA_FFCTRL(1);

	M4_REG(M4_IMASK) = 0;
	gmac_macwrite(MAC_CFG1, gmac_macread(MAC_CFG1) & ~MAC_CFG1_RXEN);
	GM_REG(GM_CTRL) &= ~GM_CTRL_RXEN;
	GM_REG(GM_CTRL) &= ~GM_CTRL_TXEN;
	gmac_macwrite(MAC_CFG1, gmac_macread(MAC_CFG1) & ~MAC_CFG1_TXEN);
	for (i = 0; i < GM_SPIN; i++)
		if ((M4_REG(M4_RXCTRL) & M4_CTRL_STALL) == 0)
			break;
	if (i == GM_SPIN)
		gmac_boot.timeouts++;
	gmac_boot.rxctrl_after = M4_REG(M4_RXCTRL);
}

void
gmac_report(void)
{

	printf("gmac0 at boot: ctrl %08x cfg1 %08x cfg2 %08x imask %08x\n",
	    gmac_boot.ctrl, gmac_boot.cfg1, gmac_boot.cfg2, gmac_boot.imask);
	printf("gmac0 at boot: m4 rx %08x head %08x tx %08x head %08x, "
	    "mc ffctrl %08x %08x\n", gmac_boot.rxctrl, gmac_boot.rxhead,
	    gmac_boot.txctrl, gmac_boot.txhead, gmac_boot.ffctrl_rx,
	    gmac_boot.ffctrl_tx);
	printf("gmac0 stopped: m4 rx %08x, %d timeouts\n",
	    gmac_boot.rxctrl_after, gmac_boot.timeouts);
}
