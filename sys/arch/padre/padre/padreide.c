/*	$NetBSD$	*/

/*
 * IT3107 SATA channels in PIO mode, through wdc(4). From padre_io.ko.
 *
 * Channel n's task file is at 0x60007000 + n * 0x100, register r in the low
 * byte of the word at +4 * r, and the alternate status at +0x38. Writes to
 * the device control register are dropped, so there is no SRST and no nIEN,
 * and the channel is polled. PIO data moves a word at a time through +0,
 * after bit 4 (data ready) or bit 5 (room) of +0x84. The low half of the
 * word holds byte 0 in bits 15:8, so storing it gives disk order, as wdc
 * expects of a stream access; IDENTIFY (ATA_DRIVE_NOSTREAM) wants the ATA
 * word values, so those are swapped.
 * RAIDiator's other path, windows at 0x60000200 + n * 0x80, never completed
 * a command.
 *
 * DMA goes through the LP processor and the DJ engine, which are not done.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/bswap.h>

#include <dev/ata/atavar.h>
#include <dev/ic/wdcvar.h>

#define	PAIDE_NCHANNELS	2	/* the Duo's two bays */
#define	PAIDE_BASE(n)	(0x60007000 + (n) * 0x100)
#define	PAIDE_ALTSTAT	0x38
#define	PAIDE_PIOSTAT	0x84
#define	  PAIDE_PIOSTAT_RDY	0x10	/* a data word can be read */
#define	  PAIDE_PIOSTAT_ROOM	0x20	/* a data word can be written */
#define	PAIDE_SPIN	100000

struct paide_softc {
	struct wdc_softc	sc_wdc;
	struct ata_channel	*sc_chp[PAIDE_NCHANNELS];
	struct ata_channel	sc_chan[PAIDE_NCHANNELS];
	struct wdc_regs		sc_regs[PAIDE_NCHANNELS];
};

static const struct padre_bus_space paide_bst = {
	.pbs_base = 0,
	.pbs_flags = PBS_REG32,
};

static int	paide_match(device_t, cfdata_t, void *);
static void	paide_attach(device_t, device_t, void *);
static void	paide_datain(struct ata_channel *, int, void *, size_t);
static void	paide_dataout(struct ata_channel *, int, void *, size_t);

CFATTACH_DECL_NEW(paide, sizeof(struct paide_softc),
    paide_match, paide_attach, NULL, NULL);

static int
paide_match(device_t parent, cfdata_t cf, void *aux)
{

	return strcmp(aux, "paide") == 0;
}

static void
paide_attach(device_t parent, device_t self, void *aux)
{
	struct paide_softc * const sc = device_private(self);
	struct wdc_regs *wdr;
	struct ata_channel *ch;
	int i, j;

	aprint_naive("\n");
	aprint_normal(": IT3107 SATA, PIO only\n");

	sc->sc_wdc.sc_atac.atac_dev = self;
	sc->sc_wdc.regs = sc->sc_regs;
	sc->sc_wdc.sc_atac.atac_cap = ATAC_CAP_DATA16 | ATAC_CAP_NOIRQ;
	sc->sc_wdc.sc_atac.atac_pio_cap = 4;
	sc->sc_wdc.sc_atac.atac_nchannels = PAIDE_NCHANNELS;
	sc->sc_wdc.sc_atac.atac_channels = sc->sc_chp;
	sc->sc_wdc.wdc_maxdrives = 1;
	sc->sc_wdc.datain_pio = paide_datain;
	sc->sc_wdc.dataout_pio = paide_dataout;

	for (i = 0; i < PAIDE_NCHANNELS; i++) {
		ch = sc->sc_chp[i] = &sc->sc_chan[i];
		wdr = &sc->sc_regs[i];
		ch->ch_channel = i;
		ch->ch_atac = &sc->sc_wdc.sc_atac;
		wdr->cmd_iot = wdr->ctl_iot = &paide_bst;
		bus_space_map(wdr->cmd_iot, PAIDE_BASE(i), 0x100, 0,
		    &wdr->cmd_baseioh);
		for (j = 0; j < WDC_NREG; j++)
			bus_space_subregion(wdr->cmd_iot, wdr->cmd_baseioh,
			    j * 4, 4, &wdr->cmd_iohs[j]);
		wdc_init_shadow_regs(wdr);
		bus_space_subregion(wdr->cmd_iot, wdr->cmd_baseioh,
		    PAIDE_ALTSTAT, 4, &wdr->ctl_ioh);
		wdcattach(ch);
	}
}

static int
paide_pio_wait(struct ata_channel *chp, uint32_t bit)
{
	struct wdc_regs * const wdr = CHAN_TO_WDC_REGS(chp);
	int i;

	for (i = 0; i < PAIDE_SPIN; i++)
		if (bus_space_read_4(wdr->cmd_iot, wdr->cmd_baseioh,
		    PAIDE_PIOSTAT) & bit)
			return 0;
	return ETIMEDOUT;
}

static void
paide_datain(struct ata_channel *chp, int flags, void *buf, size_t len)
{
	struct wdc_regs * const wdr = CHAN_TO_WDC_REGS(chp);
	const bool swap = (flags & ATA_DRIVE_NOSTREAM) != 0;
	uint16_t *p = buf;
	uint16_t w;

	for (; len >= 2; len -= 2) {
		if (paide_pio_wait(chp, PAIDE_PIOSTAT_RDY) != 0) {
			aprint_error_dev(chp->ch_atac->atac_dev,
			    "channel %d: PIO read timed out\n", chp->ch_channel);
			return;
		}
		w = bus_space_read_4(wdr->cmd_iot, wdr->cmd_baseioh, 0);
		*p++ = swap ? bswap16(w) : w;
	}
}

static void
paide_dataout(struct ata_channel *chp, int flags, void *buf, size_t len)
{
	struct wdc_regs * const wdr = CHAN_TO_WDC_REGS(chp);
	const bool swap = (flags & ATA_DRIVE_NOSTREAM) != 0;
	const uint16_t *p = buf;

	for (; len >= 2; len -= 2) {
		if (paide_pio_wait(chp, PAIDE_PIOSTAT_ROOM) != 0) {
			aprint_error_dev(chp->ch_atac->atac_dev,
			    "channel %d: PIO write timed out\n", chp->ch_channel);
			return;
		}
		bus_space_write_4(wdr->cmd_iot, wdr->cmd_baseioh, 0,
		    swap ? bswap16(*p) : *p);
		p++;
	}
}
