/*	$NetBSD$	*/

/*
 * IT3107 SATA channels through wdc(4). From padre_io.ko and the LP firmware.
 *
 * Channel n's task file is at 0x60007000 + n * 0x100, register r in the low
 * byte of the word at +4 * r, and the alternate status at +0x38. Writes to
 * the device control register are dropped, so there is no SRST and no nIEN,
 * and the channel is polled. Resets are a COMRESET through the SATA control
 * register instead, which also clears whatever command RAIDiator left
 * running (an X-RAID resync is often under way when the kernel takes over). PIO data moves a word at a time through +0,
 * after bit 4 (data ready) or bit 5 (room) of +0x84. The low half of the
 * word holds byte 0 in bits 15:8, so storing it gives disk order, as wdc
 * expects of a stream access; IDENTIFY (ATA_DRIVE_NOSTREAM) wants the ATA
 * word values, so those are swapped.
 * RAIDiator's other path, windows at 0x60000200 + n * 0x80, never completed
 * a command.
 *
 * RAIDiator hands DMA commands to the LP processor, whose firmware arms the
 * channel's MC DMA engine (MC channel n + 4: FIFO control, address, length)
 * and then writes the task file. For a command outside X-RAID it first sets
 * the channel's DJ register to 0x90, which takes the channel off the RAID
 * engine; RAIDiator may leave a channel on it in the middle of a resync. The
 * driver does the same from the CPU and leaves LP alone. The engine takes one physically contiguous buffer, so
 * transfers bounce through one per channel. Completion is polled: the engine
 * is idle and the drive is not busy.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/bswap.h>

#include <uvm/uvm_extern.h>

#include <dev/ata/atareg.h>
#include <dev/ata/atavar.h>
#include <dev/ic/wdcvar.h>

#define	PAIDE_NCHANNELS	2	/* the Duo's two bays */
#define	PAIDE_BASE(n)	(0x60007000 + (n) * 0x100)
#define	PAIDE_ALTSTAT	0x38
#define	PAIDE_PIOSTAT	0x84
#define	  PAIDE_PIOSTAT_RDY	0x10	/* a data word can be read */
#define	  PAIDE_PIOSTAT_ROOM	0x20	/* a data word can be written */
#define	PAIDE_SPIN	100000
#define	PAIDE_SETTLE	60	/* commands after a reset, 50 ms apart */
#define	PAIDE_SSTATUS	0x40
#define	  SSTATUS_UP		0x113	/* device present, Gen1 link, active */
#define	PAIDE_SCONTROL	0x48
#define	  SCONTROL_COMRESET	0x11
#define	  SCONTROL_IDLE		0x10
#define	PAIDE_PHY0	0x80	/* PHY setup values from padre_io */
#define	  PHY0_INTR_EN		0x01c00000	/* padre_set_sata_intr */
#define	  PHY0_INTR_ACK		0x1c000000
#define	PAIDE_PHY1	0x88

#define	PAIDE_MCDMA	(0x60002800 + 4 * 0x10)	/* MC DMA channel of SATA 0 */
#define	PAIDE_MCDMA_CH(n)	((n) * 0x10)
#define	MCDMA_FFCTRL	0x0
#define	  MCDMA_FFCTRL_EN	0x8000
#define	  MCDMA_FFCTRL_ACT	0x4000
#define	  MCDMA_FFCTRL_CFG	0x7fff	/* FIFO base and size, kept on reset */
#define	MCDMA_ADDR	0x8
#define	MCDMA_LEN	0xc
#define	  MCDMA_LEN_WRT		0x01000000	/* device to memory */
#define	  MCDMA_LEN_EOP		0x02000000

#define	PAIDE_DJ_CH(n)	(0x60005008 + (n) * 4)	/* DJ routing of SATA n */
#define	  DJ_CH_DIRECT		0x90

/* cf_flags */
#define	PAIDE_NODMA	0x01

struct paide_dma {
	bus_dmamap_t	pd_map;
	void		*pd_kva;	/* the bounce buffer */
	void		*pd_buf;	/* the transfer's own buffer */
	size_t		pd_len;
	bool		pd_read;
};

struct paide_softc {
	struct wdc_softc	sc_wdc;
	struct ata_channel	*sc_chp[PAIDE_NCHANNELS];
	struct ata_channel	sc_chan[PAIDE_NCHANNELS];
	struct wdc_regs		sc_regs[PAIDE_NCHANNELS];
	bus_space_handle_t	sc_mch;
	bus_space_handle_t	sc_djh;
	bus_dma_tag_t		sc_dmat;
	struct paide_dma	sc_dma[PAIDE_NCHANNELS];
};

static const struct padre_bus_space paide_bst = {
	.pbs_base = 0,
	.pbs_flags = PBS_REG32,
};

static int	paide_match(device_t, cfdata_t, void *);
static void	paide_attach(device_t, device_t, void *);
static void	paide_datain(struct ata_channel *, int, void *, size_t);
static void	paide_dataout(struct ata_channel *, int, void *, size_t);
static void	paide_reset(struct ata_channel *, int);
static int	paide_dma_alloc(struct paide_softc *, int);
static void	paide_set_modes(struct ata_channel *);
static int	paide_dma_init(void *, int, int, void *, size_t, int);
static void	paide_dma_start(void *, int, int);
static int	paide_dma_finish(void *, int, int, int);

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
	bool dma;
	int i, j;

	sc->sc_dmat = &padre_bus_dma_tag;
	dma = (device_cfdata(self)->cf_flags & PAIDE_NODMA) == 0 &&
	    bus_space_map(&paide_bst, PAIDE_MCDMA,
	    PAIDE_MCDMA_CH(PAIDE_NCHANNELS), 0, &sc->sc_mch) == 0 &&
	    bus_space_map(&paide_bst, PAIDE_DJ_CH(0), PAIDE_NCHANNELS * 4, 0,
	    &sc->sc_djh) == 0;
	for (i = 0; i < PAIDE_NCHANNELS && dma; i++)
		dma = paide_dma_alloc(sc, i) == 0;

	aprint_naive("\n");
	aprint_normal(": IT3107 SATA, %s\n", dma ? "DMA" : "PIO only");
	if (dma)	/* XXX bring-up: what RAIDiator left */
		for (i = 0; i < PAIDE_NCHANNELS; i++)
			aprint_normal_dev(self, "channel %d: DJ %#x, MC %#x, "
			    "phy %#x %#x, sstatus %#x, LP busy %#x\n", i,
			    bus_space_read_4(&paide_bst, sc->sc_djh, i * 4),
			    bus_space_read_4(&paide_bst, sc->sc_mch,
			    PAIDE_MCDMA_CH(i) + MCDMA_FFCTRL),
			    *(volatile uint32_t *)(PAIDE_BASE(i) + PAIDE_PHY0),
			    *(volatile uint32_t *)(PAIDE_BASE(i) + PAIDE_PHY1),
			    *(volatile uint32_t *)(PAIDE_BASE(i) + PAIDE_SSTATUS),
			    *(volatile uint32_t *)0x60001008);

	sc->sc_wdc.sc_atac.atac_dev = self;
	sc->sc_wdc.regs = sc->sc_regs;
	sc->sc_wdc.sc_atac.atac_cap = ATAC_CAP_DATA16 | ATAC_CAP_NOIRQ;
	sc->sc_wdc.sc_atac.atac_pio_cap = 4;
	sc->sc_wdc.sc_atac.atac_nchannels = PAIDE_NCHANNELS;
	sc->sc_wdc.sc_atac.atac_channels = sc->sc_chp;
	sc->sc_wdc.wdc_maxdrives = 1;
	sc->sc_wdc.datain_pio = paide_datain;
	sc->sc_wdc.dataout_pio = paide_dataout;
	sc->sc_wdc.reset = paide_reset;
	if (dma) {
		sc->sc_wdc.sc_atac.atac_cap |= ATAC_CAP_DMA | ATAC_CAP_UDMA;
		sc->sc_wdc.sc_atac.atac_dma_cap = 2;
		sc->sc_wdc.sc_atac.atac_udma_cap = 6;
		sc->sc_wdc.sc_atac.atac_set_modes = paide_set_modes;
		sc->sc_wdc.dma_arg = sc;
		sc->sc_wdc.dma_init = paide_dma_init;
		sc->sc_wdc.dma_start = paide_dma_start;
		sc->sc_wdc.dma_finish = paide_dma_finish;
	}

	for (i = 0; i < PAIDE_NCHANNELS; i++) {
		ch = sc->sc_chp[i] = &sc->sc_chan[i];
		wdr = &sc->sc_regs[i];
		ch->ch_channel = i;
		ch->ch_atac = &sc->sc_wdc.sc_atac;
		ch->ch_flags |= ATACH_DMA_BEFORE_CMD;
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

/*
 * padre_io's sata_init_chan: PHY setup, COMRESET, and a wait for the link.
 * wdc then waits for the drive to drop BSY.
 */
static void
paide_reset(struct ata_channel *chp, int poll)
{
	struct wdc_regs * const wdr = CHAN_TO_WDC_REGS(chp);
	const uint32_t phy0 = bus_space_read_4(wdr->cmd_iot, wdr->cmd_baseioh,
	    PAIDE_PHY0);
	int i, j;

	bus_space_write_4(wdr->cmd_iot, wdr->cmd_baseioh, PAIDE_PHY1, 0x80);
	bus_space_write_4(wdr->cmd_iot, wdr->cmd_baseioh, PAIDE_PHY0, 0x40000);
	bus_space_write_4(wdr->cmd_iot, wdr->cmd_baseioh, PAIDE_SCONTROL,
	    SCONTROL_COMRESET);
	delay(5000);
	bus_space_write_4(wdr->cmd_iot, wdr->cmd_baseioh, PAIDE_SCONTROL,
	    SCONTROL_IDLE);
	delay(5000);
	bus_space_write_4(wdr->cmd_iot, wdr->cmd_baseioh, PAIDE_PHY0, 0x48000);
	delay(5000);
	/*
	 * Keep the other bits RAIDiator set there, with the channel's
	 * interrupts off as padre_set_sata_intr(ch, 0) leaves them, so that
	 * nothing but the polling driver takes a completion.
	 */
	bus_space_write_4(wdr->cmd_iot, wdr->cmd_baseioh, PAIDE_PHY0,
	    (phy0 | PHY0_INTR_ACK) & ~PHY0_INTR_EN);
	for (i = 0; i < 1000; i++) {
		if (bus_space_read_4(wdr->cmd_iot, wdr->cmd_baseioh,
		    PAIDE_SSTATUS) == SSTATUS_UP)
			break;
		delay(1000);
	}
	if (i == 1000) {
		aprint_error_dev(chp->ch_atac->atac_dev,
		    "channel %d: no link after COMRESET, sstatus %#x\n",
		    chp->ch_channel, bus_space_read_4(wdr->cmd_iot,
		    wdr->cmd_baseioh, PAIDE_SSTATUS));
		return;
	}
	/*
	 * XXX Settle the drive. Right after the reset, the first command
	 * may stay busy for tens of ms, and the SET FEATURES of the mode
	 * probe that follows often timed out on one drive or the other,
	 * leaving it at PIO 0 or without Ultra-DMA. Presumably the drive is
	 * still busy with what RAIDiator left it (a resync is often under
	 * way). With commands spread over a few seconds here the probe
	 * passed on both drives. The command sets PIO 4, which SATA drives
	 * take, and the probe sets the modes again afterwards.
	 */
	for (i = 0; i < PAIDE_SETTLE; i++) {
		delay(50000);
		bus_space_write_1(wdr->cmd_iot, wdr->cmd_iohs[wd_sdh], 0,
		    WDSD_IBM);
		bus_space_write_1(wdr->cmd_iot, wdr->cmd_iohs[wd_features], 0,
		    WDSF_SET_MODE);
		bus_space_write_1(wdr->cmd_iot, wdr->cmd_iohs[wd_seccnt], 0,
		    0x08 | 4);
		bus_space_write_1(wdr->cmd_iot, wdr->cmd_iohs[wd_command], 0,
		    SET_FEATURES);
		delay(10);
		for (j = 0; j < 5000; j++) {
			if ((bus_space_read_1(wdr->ctl_iot, wdr->ctl_ioh, 0) &
			    (WDCS_BSY | WDCS_DRQ)) == 0)
				break;
			delay(1000);
		}
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

static int
paide_dma_alloc(struct paide_softc *sc, int n)
{
	struct paide_dma * const pd = &sc->sc_dma[n];
	bus_dma_segment_t seg;
	int error, rseg;

	error = bus_dmamem_alloc(sc->sc_dmat, MAXPHYS, PAGE_SIZE, 0, &seg, 1,
	    &rseg, BUS_DMA_WAITOK);
	if (error != 0)
		return error;
	error = bus_dmamem_map(sc->sc_dmat, &seg, 1, MAXPHYS, &pd->pd_kva,
	    BUS_DMA_WAITOK);
	if (error == 0)
		error = bus_dmamap_create(sc->sc_dmat, MAXPHYS, 1, MAXPHYS, 0,
		    BUS_DMA_WAITOK, &pd->pd_map);
	if (error == 0)
		error = bus_dmamap_load_raw(sc->sc_dmat, pd->pd_map, &seg, 1,
		    MAXPHYS, BUS_DMA_WAITOK);
	return error;
}

/* The channels have no timing to set; wdc sends the drive SET FEATURES. */
static void
paide_set_modes(struct ata_channel *chp)
{
}

static int
paide_dma_init(void *v, int channel, int drive, void *databuf, size_t datalen,
    int flags)
{
	struct paide_softc * const sc = v;
	struct paide_dma * const pd = &sc->sc_dma[channel];

	if (datalen > MAXPHYS)
		return EINVAL;	/* wdc falls back to PIO */
	pd->pd_buf = databuf;
	pd->pd_len = datalen;
	pd->pd_read = (flags & WDC_DMA_READ) != 0;
	if (pd->pd_read)
		bus_dmamap_sync(sc->sc_dmat, pd->pd_map, 0, datalen,
		    BUS_DMASYNC_PREREAD);
	else {
		memcpy(pd->pd_kva, databuf, datalen);
		bus_dmamap_sync(sc->sc_dmat, pd->pd_map, 0, datalen,
		    BUS_DMASYNC_PREWRITE);
	}
	return 0;
}

/* Clearing the enable bit and setting it again empties the FIFO. */
static void
paide_dma_reset(struct paide_softc *sc, int channel)
{
	const bus_size_t o = PAIDE_MCDMA_CH(channel) + MCDMA_FFCTRL;
	const uint32_t cfg = bus_space_read_4(&paide_bst, sc->sc_mch, o) &
	    MCDMA_FFCTRL_CFG;

	bus_space_write_4(&paide_bst, sc->sc_mch, o, cfg);
	bus_space_write_4(&paide_bst, sc->sc_mch, o, cfg | MCDMA_FFCTRL_EN);
}

/*
 * As LP does before a command: route the channel past DJ, wait for the
 * engine, reset the FIFO, arm.
 */
static void
paide_dma_start(void *v, int channel, int drive)
{
	struct paide_softc * const sc = v;
	struct paide_dma * const pd = &sc->sc_dma[channel];
	const bus_size_t ch = PAIDE_MCDMA_CH(channel);
	int i;

	bus_space_write_4(&paide_bst, sc->sc_djh, channel * 4, DJ_CH_DIRECT);
	for (i = 0; i < PAIDE_SPIN; i++)
		if ((bus_space_read_4(&paide_bst, sc->sc_mch,
		    ch + MCDMA_FFCTRL) & MCDMA_FFCTRL_ACT) == 0)
			break;
	if (i == PAIDE_SPIN)
		aprint_error_dev(sc->sc_wdc.sc_atac.atac_dev,
		    "channel %d: DMA engine busy\n", channel);
	paide_dma_reset(sc, channel);
	bus_space_write_4(&paide_bst, sc->sc_mch, ch + MCDMA_ADDR,
	    pd->pd_map->dm_segs[0].ds_addr);
	bus_space_write_4(&paide_bst, sc->sc_mch, ch + MCDMA_LEN,
	    pd->pd_len | MCDMA_LEN_EOP | (pd->pd_read ? MCDMA_LEN_WRT : 0));
}

static int
paide_dma_finish(void *v, int channel, int drive, int force)
{
	struct paide_softc * const sc = v;
	struct paide_dma * const pd = &sc->sc_dma[channel];
	struct wdc_regs * const wdr = &sc->sc_regs[channel];

	if (force == WDC_DMAEND_END) {
		if ((bus_space_read_4(&paide_bst, sc->sc_mch,
		    PAIDE_MCDMA_CH(channel) + MCDMA_FFCTRL) &
		    MCDMA_FFCTRL_ACT) != 0 ||
		    (bus_space_read_1(wdr->ctl_iot, wdr->ctl_ioh, 0) &
		    WDCS_BSY) != 0)
			return WDC_DMAST_NOIRQ;
	} else {
		/* XXX bring-up: the state of a transfer that did not finish */
		aprint_error_dev(sc->sc_wdc.sc_atac.atac_dev,
		    "channel %d: DMA abort, MC %#x %#x %#x, st %#x, pio %#x, "
		    "sstatus %#x serror %#x\n", channel,
		    bus_space_read_4(&paide_bst, sc->sc_mch,
		    PAIDE_MCDMA_CH(channel) + MCDMA_FFCTRL),
		    bus_space_read_4(&paide_bst, sc->sc_mch,
		    PAIDE_MCDMA_CH(channel) + MCDMA_ADDR),
		    bus_space_read_4(&paide_bst, sc->sc_mch,
		    PAIDE_MCDMA_CH(channel) + MCDMA_LEN),
		    bus_space_read_1(wdr->ctl_iot, wdr->ctl_ioh, 0),
		    bus_space_read_4(wdr->cmd_iot, wdr->cmd_baseioh,
		    PAIDE_PIOSTAT),
		    bus_space_read_4(wdr->cmd_iot, wdr->cmd_baseioh, 0x40),
		    bus_space_read_4(wdr->cmd_iot, wdr->cmd_baseioh, 0x44));
		paide_dma_reset(sc, channel);
	}
	if (pd->pd_read) {
		bus_dmamap_sync(sc->sc_dmat, pd->pd_map, 0, pd->pd_len,
		    BUS_DMASYNC_POSTREAD);
		if (force == WDC_DMAEND_END)
			memcpy(pd->pd_buf, pd->pd_kva, pd->pd_len);
	}
	return 0;
}
