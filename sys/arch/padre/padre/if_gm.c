/*	$NetBSD$	*/

/*
 * IT3107 GMAC0, see gmacreg.h.
 *
 * The M4 engine did not take descriptors from DDR, so the rings are where
 * padre_gmac had them, in S3 SRAM; buffers are non-cached DDR, so neither
 * needs cache maintenance. Received frames, which the GMAC writes 2 bytes
 * into the buffer, are copied into mbufs at ETHER_ALIGN, since SPARC traps
 * on misaligned loads. Transmitted frames are copied into a buffer per
 * slot, one descriptor per frame.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/device.h>
#include <sys/intr.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_ether.h>
#include <net/if_media.h>
#include <net/bpf.h>

#include <dev/mii/mii.h>
#include <dev/mii/miivar.h>

#include <padre/padre/gmacreg.h>

#define	GM_NRXDESC	64
#define	GM_NTXDESC	64
#define	GM_BUFSZ	2048
#define	GM_RXBUFLEN	0x600	/* padre_gmac's default */
#define	GM_RXPAD	2	/* the GMAC writes 2 bytes ahead of each frame */
#define	GM_NEXTRX(i)	(((i) + 1) % GM_NRXDESC)
#define	GM_NEXTTX(i)	(((i) + 1) % GM_NTXDESC)

/* padre_gmac's descriptor area in S3 SRAM, reached non-cached here. */
#define	GM_DESC_PA	0x30001000
#define	GM_DESC_VA	PMAP_DIRECT_VA(GM_DESC_PA)

/* iboot's boot_param, which holds the MAC address at offset 8. */
#define	BOOT_PARAM_MAC	((const uint8_t *)0x80000008)

struct gm_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_gmh;
	bus_space_handle_t	sc_m4h;
	bus_dma_tag_t		sc_dmat;
	struct ethercom		sc_ec;
	struct mii_data		sc_mii;
	callout_t		sc_tick;
	void			*sc_ih;
	uint8_t			sc_enaddr[ETHER_ADDR_LEN];

	struct m4_desc		*sc_rxd;
	struct m4_desc		*sc_txd;
	bus_dmamap_t		sc_bufmap;
	uint8_t			*sc_rxbuf;
	uint8_t			*sc_txbuf;

	int			sc_rxptr;
	int			sc_txnext;	/* next slot to fill */
	int			sc_txdirty;	/* oldest slot in flight */
	int			sc_txfree;
};

#define	GM_READ(sc, o)		bus_space_read_4((sc)->sc_bst, (sc)->sc_gmh, (o))
#define	GM_WRITE(sc, o, v)	bus_space_write_4((sc)->sc_bst, (sc)->sc_gmh, (o), (v))
#define	M4_READ(sc, o)		bus_space_read_4((sc)->sc_bst, (sc)->sc_m4h, (o))
#define	M4_WRITE(sc, o, v)	bus_space_write_4((sc)->sc_bst, (sc)->sc_m4h, (o), (v))
#define	REG(a)			(*(volatile uint32_t *)(a))

#define	GM_DESCADDR(sc, p)	(GM_DESC_PA + \
				    ((uint8_t *)(p) - (uint8_t *)(sc)->sc_rxd))
#define	GM_BUFADDR(sc, p)	((sc)->sc_bufmap->dm_segs[0].ds_addr + \
				    ((uint8_t *)(p) - (sc)->sc_rxbuf))

/* Register addresses are CPU addresses on padre. */
static const struct padre_bus_space gm_bst = { .pbs_base = 0 };

static int	gm_match(device_t, cfdata_t, void *);
static void	gm_attach(device_t, device_t, void *);
static int	gm_alloc(struct gm_softc *);
static int	gm_init(struct ifnet *);
static void	gm_stop(struct ifnet *, int);
static void	gm_start(struct ifnet *);
static int	gm_ioctl(struct ifnet *, u_long, void *);
static void	gm_watchdog(struct ifnet *);
static int	gm_intr(void *);
static void	gm_rxintr(struct gm_softc *);
static void	gm_txintr(struct gm_softc *);
static void	gm_tick(void *);
static void	gm_reset(struct gm_softc *);
static void	gm_setfilter(struct gm_softc *);
static int	gm_mii_readreg(device_t, int, int, uint16_t *);
static int	gm_mii_writereg(device_t, int, int, uint16_t);
static void	gm_mii_statchg(struct ifnet *);

CFATTACH_DECL_NEW(gm, sizeof(struct gm_softc),
    gm_match, gm_attach, NULL, NULL);

static int
gm_match(device_t parent, cfdata_t cf, void *aux)
{

	return strcmp(aux, "gm") == 0;
}

static void
gm_attach(device_t parent, device_t self, void *aux)
{
	struct gm_softc * const sc = device_private(self);
	struct ifnet * const ifp = &sc->sc_ec.ec_if;
	struct mii_data * const mii = &sc->sc_mii;
	uint32_t s1, s2;

	sc->sc_dev = self;
	sc->sc_bst = &gm_bst;
	sc->sc_dmat = &padre_bus_dma_tag;
	bus_space_map(sc->sc_bst, GM0_BASE, 0x1000, 0, &sc->sc_gmh);
	bus_space_map(sc->sc_bst, M4_BASE, 0x100, 0, &sc->sc_m4h);

	/* boot_param first; otherwise what RAIDiator left in the MAC. */
	memcpy(sc->sc_enaddr, BOOT_PARAM_MAC, ETHER_ADDR_LEN);
	if (ETHER_IS_MULTICAST(sc->sc_enaddr) ||
	    memcmp(sc->sc_enaddr, "\0\0\0\0\0\0", ETHER_ADDR_LEN) == 0) {
		s1 = gmac_macread(MAC_STATION1);
		s2 = gmac_macread(MAC_STATION2);
		sc->sc_enaddr[0] = s2 >> 16;
		sc->sc_enaddr[1] = s2 >> 24;
		sc->sc_enaddr[2] = s1;
		sc->sc_enaddr[3] = s1 >> 8;
		sc->sc_enaddr[4] = s1 >> 16;
		sc->sc_enaddr[5] = s1 >> 24;
	}

	aprint_naive("\n");
	aprint_normal(": IT3107 GMAC\n");

	if (gm_alloc(sc) != 0)
		return;

	callout_init(&sc->sc_tick, 0);
	callout_setfunc(&sc->sc_tick, gm_tick, sc);

	strlcpy(ifp->if_xname, device_xname(self), IFNAMSIZ);
	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = gm_ioctl;
	ifp->if_start = gm_start;
	ifp->if_init = gm_init;
	ifp->if_stop = gm_stop;
	ifp->if_watchdog = gm_watchdog;
	IFQ_SET_MAXLEN(&ifp->if_snd, GM_NTXDESC - 1);
	IFQ_SET_READY(&ifp->if_snd);

	/* The MDIO clock has to be set before the PHYs can be probed. */
	gmac_macwrite(MAC_MIIMGMT_CFG, MAC_MIIMGMT_CFG_INIT);

	sc->sc_ec.ec_mii = mii;
	mii->mii_ifp = ifp;
	mii->mii_readreg = gm_mii_readreg;
	mii->mii_writereg = gm_mii_writereg;
	mii->mii_statchg = gm_mii_statchg;
	ifmedia_init(&mii->mii_media, 0, ether_mediachange, ether_mediastatus);
	mii_attach(self, mii, 0xffffffff, MII_PHY_ANY, MII_OFFSET_ANY, 0);
	if (LIST_EMPTY(&mii->mii_phys)) {
		aprint_error_dev(self, "no PHY found\n");
		ifmedia_add(&mii->mii_media, IFM_ETHER | IFM_NONE, 0, NULL);
		ifmedia_set(&mii->mii_media, IFM_ETHER | IFM_NONE);
	} else
		ifmedia_set(&mii->mii_media, IFM_ETHER | IFM_AUTO);

	if_initialize(ifp);
	ifp->if_percpuq = if_percpuq_create(ifp);
	if_deferred_start_init(ifp, NULL);
	ether_ifattach(ifp, sc->sc_enaddr);
	if_register(ifp);

	sc->sc_ih = padre_intr_establish(GM0_IRQ, IPL_NET, gm_intr, sc);
}

static int
gm_alloc(struct gm_softc *sc)
{
	const bus_size_t bsize = (GM_NRXDESC + GM_NTXDESC) * GM_BUFSZ;
	bus_dma_segment_t seg;
	int rseg, error;
	void *kva;

	sc->sc_rxd = (struct m4_desc *)GM_DESC_VA;
	sc->sc_txd = sc->sc_rxd + GM_NRXDESC;

	if ((error = bus_dmamem_alloc(sc->sc_dmat, bsize, PAGE_SIZE, 0, &seg,
	    1, &rseg, BUS_DMA_WAITOK)) != 0 ||
	    (error = bus_dmamem_map(sc->sc_dmat, &seg, rseg, bsize, &kva,
	    BUS_DMA_WAITOK | BUS_DMA_COHERENT)) != 0 ||
	    (error = bus_dmamap_create(sc->sc_dmat, bsize, 1, bsize, 0,
	    BUS_DMA_WAITOK, &sc->sc_bufmap)) != 0 ||
	    (error = bus_dmamap_load(sc->sc_dmat, sc->sc_bufmap, kva, bsize,
	    NULL, BUS_DMA_WAITOK)) != 0)
		goto fail;
	sc->sc_rxbuf = kva;
	sc->sc_txbuf = sc->sc_rxbuf + GM_NRXDESC * GM_BUFSZ;
	return 0;

fail:
	aprint_error_dev(sc->sc_dev, "cannot allocate DMA memory, error %d\n",
	    error);
	return error;
}

/* padre_gmac's reset, without checksum offload. */
static void
gm_reset(struct gm_softc *sc)
{
	struct ifnet * const ifp = &sc->sc_ec.ec_if;
	uint32_t v;

	/* The MC DMA FIFO for TX, and the GMAC's pulse in DJ. */
	v = REG(MC_DMA_FFCTRL(1));
	REG(MC_DMA_FFCTRL(1)) = v & ~MC_DMA_FFCTRL_EN;
	REG(MC_DMA_FFCTRL(1)) = v | MC_DMA_FFCTRL_EN;
	v = REG(DJ_RESET);
	REG(DJ_CTRL) |= DJ_CTRL_GM0;
	REG(DJ_RESET) = v | DJ_RESET_GM0;
	delay(1);
	REG(DJ_RESET) = v;

	GM_WRITE(sc, GM_CTRL, GM_READ(sc, GM_CTRL) | GM_CTRL_RESET);
	delay(100000);
	GM_WRITE(sc, GM_CTRL, GM_READ(sc, GM_CTRL) & ~GM_CTRL_RESET);
	while (GM_READ(sc, GM_CTRL) & GM_CTRL_RXFIFO)
		(void)GM_READ(sc, GM_RXFIFO);

	gmac_macwrite(MAC_CFG1, MAC_CFG1_RESET);
	delay(100);
	gmac_macwrite(MAC_CFG1, gmac_macread(MAC_CFG1) & ~MAC_CFG1_RESET);
	delay(100);
	gmac_macwrite(MAC_MAXFRAME, ifp->if_mtu + 36);
	gmac_macwrite(MAC_CFG1, gmac_macread(MAC_CFG1) | MAC_CFG1_TXEN);
	gmac_macwrite(MAC_CFG2, MAC_CFG2_INIT);
	delay(1);
	gmac_macwrite(MAC_MIIMGMT_CFG, MAC_MIIMGMT_CFG_INIT);
	delay(1);
	GM_WRITE(sc, GM_CTRL, GM_CTRL_INIT);
	GM_WRITE(sc, GM_CTRL2, GM_READ(sc, GM_CTRL2) | GM_CTRL2_INIT);

	/* No segmentation offload. */
	REG(GMT0_CTRL) = 0;

	gmac_macwrite(MAC_STATION1, sc->sc_enaddr[5] << 24 |
	    sc->sc_enaddr[4] << 16 | sc->sc_enaddr[3] << 8 | sc->sc_enaddr[2]);
	gmac_macwrite(MAC_STATION2, sc->sc_enaddr[1] << 24 |
	    sc->sc_enaddr[0] << 16);
}

static void
gm_setfilter(struct gm_softc *sc)
{
	struct ifnet * const ifp = &sc->sc_ec.ec_if;
	uint32_t ctrl;

	/* All multicast until the hash function is worked out. */
	ctrl = GM_READ(sc, GM_CTRL) | GM_CTRL_MCAST;
	if (ifp->if_flags & IFF_PROMISC)
		ctrl |= GM_CTRL_PROMISC;
	else
		ctrl &= ~GM_CTRL_PROMISC;
	GM_WRITE(sc, GM_HASHH, 0xffffffff);
	GM_WRITE(sc, GM_HASHL, 0xffffffff);
	GM_WRITE(sc, GM_CTRL, ctrl);
	ifp->if_flags |= IFF_ALLMULTI;
}

static int
gm_init(struct ifnet *ifp)
{
	struct gm_softc * const sc = ifp->if_softc;
	struct m4_desc *d;
	uint32_t v;
	int i, s;

	s = splnet();
	gm_stop(ifp, 0);
	gm_reset(sc);

	M4_WRITE(sc, M4_TXCTRL, M4_CTRL_RESET);
	M4_WRITE(sc, M4_RXCTRL, M4_CTRL_RESET);
	M4_WRITE(sc, M4_FLOCNT, M4_FLOCNT_INIT);
	M4_WRITE(sc, M4_FLOCFG, M4_FLOCFG_INIT);

	for (i = 0; i < GM_NRXDESC; i++) {
		d = &sc->sc_rxd[i];
		d->d_link = GM_DESCADDR(sc, &sc->sc_rxd[GM_NEXTRX(i)]);
		d->d_buf = GM_BUFADDR(sc, sc->sc_rxbuf + i * GM_BUFSZ);
		d->d_stat = 0;
		d->d_ctrl = M4D_OWNER | M4D_SOP | M4D_INTR0 | GM_RXBUFLEN;
	}
	for (i = 0; i < GM_NTXDESC; i++) {
		d = &sc->sc_txd[i];
		d->d_link = GM_DESCADDR(sc, &sc->sc_txd[GM_NEXTTX(i)]);
		d->d_buf = GM_BUFADDR(sc, sc->sc_txbuf + i * GM_BUFSZ);
		d->d_stat = 0;
		d->d_ctrl = 0;
	}
	sc->sc_rxptr = 0;
	sc->sc_txnext = sc->sc_txdirty = 0;
	sc->sc_txfree = GM_NTXDESC;
	M4_WRITE(sc, M4_RXFILTL, 0);
	M4_WRITE(sc, M4_RXFILTH, 0);
	M4_WRITE(sc, M4_RXHEAD, GM_DESCADDR(sc, &sc->sc_rxd[0]));
	M4_WRITE(sc, M4_TXHEAD, GM_DESCADDR(sc, &sc->sc_txd[0]));

	gm_setfilter(sc);
	mii_mediachg(&sc->sc_mii);

	/* padre_gmac_init_rx_mc and padre_gmac_start_dma. */
	v = REG(MC_DMA_FFCTRL(0));
	REG(MC_DMA_FFCTRL(0)) = v & ~MC_DMA_FFCTRL_EN;
	REG(MC_DMA_FFCTRL(0)) = v | MC_DMA_FFCTRL_EN;
	M4_WRITE(sc, M4_RXCTRL, M4_CTRL_GO);
	GM_WRITE(sc, GM_CTRL, GM_READ(sc, GM_CTRL) | GM_CTRL_RXEN);
	gmac_macwrite(MAC_CFG1, gmac_macread(MAC_CFG1) | MAC_CFG1_RXEN);
	gmac_macwrite(MAC_CFG1, gmac_macread(MAC_CFG1) | MAC_CFG1_TXEN);
	GM_WRITE(sc, GM_CTRL, GM_READ(sc, GM_CTRL) | GM_CTRL_TXEN);
	while (GM_READ(sc, GM_CTRL) & GM_CTRL_RXFIFO)
		(void)GM_READ(sc, GM_RXFIFO);
	M4_WRITE(sc, M4_TXCTRL, M4_CTRL_GO);
	M4_WRITE(sc, M4_IMASK, M4_INT_RX | M4_INT_TX);

	ifp->if_flags |= IFF_RUNNING;
	callout_schedule(&sc->sc_tick, hz);
	splx(s);
	return 0;
}

/* padre_gmac_stop_dma, as gmac_quiesce does at boot. */
static void
gm_stop(struct ifnet *ifp, int disable)
{
	struct gm_softc * const sc = ifp->if_softc;
	int i;

	callout_stop(&sc->sc_tick);
	M4_WRITE(sc, M4_IMASK, 0);
	gmac_macwrite(MAC_CFG1, gmac_macread(MAC_CFG1) & ~MAC_CFG1_RXEN);
	GM_WRITE(sc, GM_CTRL, GM_READ(sc, GM_CTRL) & ~GM_CTRL_RXEN);
	GM_WRITE(sc, GM_CTRL, GM_READ(sc, GM_CTRL) & ~GM_CTRL_TXEN);
	gmac_macwrite(MAC_CFG1, gmac_macread(MAC_CFG1) & ~MAC_CFG1_TXEN);
	for (i = 0; i < 1000; i++) {
		if ((M4_READ(sc, M4_RXCTRL) & M4_CTRL_STALL) == 0)
			break;
		delay(10);
	}
	mii_down(&sc->sc_mii);
	ifp->if_flags &= ~IFF_RUNNING;
	ifp->if_timer = 0;
}

static void
gm_start(struct ifnet *ifp)
{
	struct gm_softc * const sc = ifp->if_softc;
	struct m4_desc *d;
	struct mbuf *m;
	uint8_t *buf;
	int len, queued = 0;

	if ((ifp->if_flags & IFF_RUNNING) == 0)
		return;

	while (sc->sc_txfree > 0) {
		IFQ_DEQUEUE(&ifp->if_snd, m);
		if (m == NULL)
			break;
		len = m->m_pkthdr.len;
		if (len > GM_BUFSZ) {
			m_freem(m);
			if_statinc(ifp, if_oerrors);
			continue;
		}
		buf = sc->sc_txbuf + sc->sc_txnext * GM_BUFSZ;
		m_copydata(m, 0, len, buf);
		bpf_mtap(ifp, m, BPF_D_OUT);
		m_freem(m);

		/* The owner bit goes in last, with the rest of the word. */
		d = &sc->sc_txd[sc->sc_txnext];
		d->d_stat = 0;
		d->d_ctrl = M4D_OWNER | M4D_SOP | M4D_EOP | M4D_INTR0 | len;
		sc->sc_txnext = GM_NEXTTX(sc->sc_txnext);
		sc->sc_txfree--;
		queued++;
	}
	if (queued == 0)
		return;
	if (M4_READ(sc, M4_TXCTRL) & M4_CTRL_STALL)
		M4_WRITE(sc, M4_TXCTRL, M4_CTRL_GO);
	ifp->if_timer = 5;
}

static void
gm_txintr(struct gm_softc *sc)
{
	struct ifnet * const ifp = &sc->sc_ec.ec_if;

	while (sc->sc_txfree < GM_NTXDESC &&
	    (sc->sc_txd[sc->sc_txdirty].d_ctrl & M4D_OWNER) == 0) {
		if (sc->sc_txd[sc->sc_txdirty].d_ctrl & M4D_ERR)
			if_statinc(ifp, if_oerrors);
		else
			if_statinc(ifp, if_opackets);
		sc->sc_txdirty = GM_NEXTTX(sc->sc_txdirty);
		sc->sc_txfree++;
	}
	if (sc->sc_txfree == GM_NTXDESC)
		ifp->if_timer = 0;
}

static void
gm_rxintr(struct gm_softc *sc)
{
	struct ifnet * const ifp = &sc->sc_ec.ec_if;
	struct m4_desc *d;
	struct mbuf *m;
	uint32_t ctrl, stat;
	int len, n = 0;

	for (;; sc->sc_rxptr = GM_NEXTRX(sc->sc_rxptr)) {
		d = &sc->sc_rxd[sc->sc_rxptr];
		ctrl = d->d_ctrl;
		if (ctrl & M4D_OWNER)
			break;
		stat = d->d_stat;
		len = stat & 0xffff;
		n++;

		if ((ctrl & (M4D_ERR | M4D_EOP)) != M4D_EOP ||
		    len < ETHER_MIN_LEN - ETHER_CRC_LEN ||
		    len > GM_RXBUFLEN - GM_RXPAD) {
			if_statinc(ifp, if_ierrors);
			goto next;
		}

		MGETHDR(m, M_DONTWAIT, MT_DATA);
		if (m != NULL && len + ETHER_ALIGN > MHLEN) {
			MCLGET(m, M_DONTWAIT);
			if ((m->m_flags & M_EXT) == 0) {
				m_freem(m);
				m = NULL;
			}
		}
		if (m == NULL) {
			if_statinc(ifp, if_ierrors);
			goto next;
		}
		m->m_data += ETHER_ALIGN;
		memcpy(mtod(m, void *),
		    sc->sc_rxbuf + sc->sc_rxptr * GM_BUFSZ + GM_RXPAD, len);
		m->m_pkthdr.len = m->m_len = len;
		m->m_flags |= M_HASFCS;
		m_set_rcvif(m, ifp);
		if_percpuq_enqueue(ifp->if_percpuq, m);

next:
		d->d_stat = 0;
		d->d_ctrl = M4D_OWNER | M4D_SOP | M4D_INTR0 | GM_RXBUFLEN;
	}

	/* padre_gmac returns the count, and restarts a stalled engine. */
	if (n > 0) {
		M4_WRITE(sc, M4_FLOCNT, n);
		if (M4_READ(sc, M4_RXCTRL) & M4_CTRL_STALL)
			M4_WRITE(sc, M4_RXCTRL, M4_CTRL_GO);
	}
}

static int
gm_intr(void *arg)
{
	struct gm_softc * const sc = arg;
	struct ifnet * const ifp = &sc->sc_ec.ec_if;
	uint32_t pend;
	int handled = 0;

	while ((pend = M4_READ(sc, M4_IPEND) &
	    (M4_INT_RX | M4_INT_TX)) != 0) {
		handled = 1;
		if (pend & M4_INT_TX) {
			gm_txintr(sc);
			M4_WRITE(sc, M4_ICLR, M4_INT_TX);
		}
		if (pend & M4_INT_RX) {
			M4_WRITE(sc, M4_ICLR, M4_INT_RX);
			gm_rxintr(sc);
		}
	}
	if (handled)
		if_schedule_deferred_start(ifp);
	return handled;
}

static void
gm_watchdog(struct ifnet *ifp)
{
	struct gm_softc * const sc = ifp->if_softc;

	printf("%s: transmit timeout, tx ctrl %08x\n", device_xname(sc->sc_dev),
	    M4_READ(sc, M4_TXCTRL));
	if_statinc(ifp, if_oerrors);
	gm_init(ifp);
}

static int
gm_ioctl(struct ifnet *ifp, u_long cmd, void *data)
{
	struct gm_softc * const sc = ifp->if_softc;
	int s, error;

	s = splnet();
	error = ether_ioctl(ifp, cmd, data);
	if (error == ENETRESET) {
		if (ifp->if_flags & IFF_RUNNING)
			gm_setfilter(sc);
		error = 0;
	}
	splx(s);
	return error;
}

static void
gm_tick(void *arg)
{
	struct gm_softc * const sc = arg;
	int s;

	s = splnet();
	mii_tick(&sc->sc_mii);
	splx(s);
	callout_schedule(&sc->sc_tick, hz);
}

static int
gm_mii_wait(void)
{
	int i;

	for (i = 0; i < 200; i++) {
		if ((gmac_macread(MAC_MIIMGMT_IND) & MAC_MIIMGMT_IND_BUSY) == 0)
			return 0;
		delay(5);
	}
	return ETIMEDOUT;
}

static int
gm_mii_readreg(device_t self, int phy, int reg, uint16_t *val)
{
	int error;

	gmac_macwrite(MAC_MIIMGMT_ADDR, phy << 8 | reg);
	gmac_macwrite(MAC_MIIMGMT_CMD, MAC_MIIMGMT_CMD_READ);
	delay(5);
	error = gm_mii_wait();
	*val = gmac_macread(MAC_MIIMGMT_STAT);
	gmac_macwrite(MAC_MIIMGMT_CMD, 0);
	return error;
}

/*
 * padre_gmac changes register 0x0c of the I2C device at 0x69, apparently a
 * clock generator, for 20-80 ms around every write to a Marvell PHY. Without
 * that, a write stored 0 in BMCR, which forces 10 Mb/s half duplex. Until
 * there is an I2C driver, writes are dropped and the PHY keeps what
 * RAIDiator set up: autonegotiation of every mode. The write itself is
 * MAC_MIIMGMT_ADDR, then the data to MAC_MIIMGMT_CTRL, then gm_mii_wait().
 */
static int
gm_mii_writereg(device_t self, int phy, int reg, uint16_t val)
{

	return 0;
}

/* PE_MCX_set_speed: the mode and duplex change with RX and TX off. */
static void
gm_mii_statchg(struct ifnet *ifp)
{
	struct gm_softc * const sc = ifp->if_softc;
	const u_int media = sc->sc_mii.mii_media_active;
	uint32_t cfg1, en, cfg2;

	cfg1 = gmac_macread(MAC_CFG1);
	en = cfg1 & (MAC_CFG1_RXEN | MAC_CFG1_TXEN);
	cfg1 &= ~en;
	if (en != 0)
		gmac_macwrite(MAC_CFG1, cfg1);
	cfg2 = gmac_macread(MAC_CFG2) & ~(MAC_CFG2_MODE | MAC_CFG2_FDX);
	if (IFM_SUBTYPE(media) == IFM_1000_T)
		cfg2 |= MAC_CFG2_MODE_GMII;
	else
		cfg2 |= MAC_CFG2_MODE_MII;
	if (media & IFM_FDX)
		cfg2 |= MAC_CFG2_FDX;
	gmac_macwrite(MAC_CFG2, cfg2);
	gmac_macwrite(MAC_CFG1, cfg1 | MAC_CFG1_RXTXRESET);
	delay(1);
	gmac_macwrite(MAC_CFG1, cfg1 | en);
}
