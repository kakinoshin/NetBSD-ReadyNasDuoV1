/*	$NetBSD$	*/

/*
 * IT3107 GMAC, reconstructed from RAIDiator's padre_gmac.ko, which carries
 * DWARF for its own structures; register offsets come from its code.
 *
 * Each GMAC is a wrapper around a PE-MCXMAC core, the MAC also found in the
 * Agere ET1310, whose registers are reached indirectly through GM_MACCMD and
 * GM_MACDATA. Frames move through the M4 block, a descriptor DMA engine
 * shared by both GMACs, and memory controller (MC) DMA channels.
 */

#ifndef _PADRE_GMACREG_H_
#define _PADRE_GMACREG_H_

#define	GM0_BASE	0x60010000
#define	GM1_BASE	0x60020000

/* Wrapper registers, from the GMAC base. */
#define	GM_CTRL		0x400
#define	  GM_CTRL_RESET		0x80000000	/* held for 100 ms */
#define	  GM_CTRL_TXEN		0x20000000
#define	  GM_CTRL_RXEN		0x10000000
#define	  GM_CTRL_PROMISC	0x08000000
#define	  GM_CTRL_MCAST		0x02000000	/* filter through GM_HASH */
#define	  GM_CTRL_INIT		0x25000300	/* after reset, no offloads */
#define	  GM_CTRL_RXFIFO	0x00001000	/* drained by reading GM_RXFIFO */
#define	GM_HASHH	0x404
#define	GM_HASHL	0x408
#define	GM_MACCMD	0x418
#define	  GM_MACCMD_REG(r)	((r) << 16)
#define	  GM_MACCMD_DONE	0x00000004
#define	  GM_MACCMD_READ	0x00000002
#define	  GM_MACCMD_GO		0x00000001
#define	GM_MACDATA	0x41c
#define	GM_RXFIFO	0x420
#define	GM_CTRL2	0x428
#define	  GM_CTRL2_INIT		0x00000201	/* set after reset */

/* PE-MCXMAC registers, through GM_MACCMD; names as in the ET1310. */
#define	MAC_CFG1	0x00
#define	  MAC_CFG1_RESET	0x80000000
#define	  MAC_CFG1_RXEN		0x00000004
#define	  MAC_CFG1_TXEN		0x00000001
#define	  MAC_CFG1_RXTXRESET	0x00030000	/* pulsed around speed changes */
#define	MAC_CFG2	0x04
#define	  MAC_CFG2_INIT		0x00007105	/* preamble 7, pad and CRC, MII */
#define	  MAC_CFG2_MODE		0x00000300
#define	  MAC_CFG2_MODE_MII	0x00000100
#define	  MAC_CFG2_MODE_GMII	0x00000200
#define	  MAC_CFG2_FDX		0x00000001
#define	MAC_MAXFRAME	0x10	/* MTU + 36 */
#define	MAC_MIIMGMT_CFG	0x20
#define	  MAC_MIIMGMT_CFG_INIT	0x00000007	/* clock divider */
#define	MAC_MIIMGMT_CMD	0x24
#define	  MAC_MIIMGMT_CMD_READ	0x00000001
#define	MAC_MIIMGMT_ADDR 0x28	/* PHY << 8 | register */
#define	MAC_MIIMGMT_CTRL 0x2c	/* write data */
#define	MAC_MIIMGMT_STAT 0x30	/* read data */
#define	MAC_MIIMGMT_IND	0x34
#define	  MAC_MIIMGMT_IND_BUSY	0x00000001
#define	MAC_STATION1	0x40
#define	MAC_STATION2	0x44

/* M4 descriptor DMA; GMAC1's registers are at +0x40, +0x10 for interrupts. */
#define	M4_BASE		0x60000700
#define	M4_RXCTRL	0x00
#define	  M4_CTRL_RESET		0x00000008
#define	  M4_CTRL_STALL		0x00000002	/* see below */
#define	  M4_CTRL_GO		0x00000001
#define	M4_RXHEAD	0x04	/* physical address of the first descriptor */
#define	M4_RXFILTH	0x08
#define	M4_RXFILTL	0x0c
#define	M4_FLOCFG	0x10
#define	  M4_FLOCFG_INIT	0x00000131
#define	M4_FLOCNT	0x14	/* frames taken from the ring */
#define	  M4_FLOCNT_INIT	0x00000180
#define	M4_TXCTRL	0x20
#define	M4_TXHEAD	0x24
#define	M4_IPEND	0xe0
#define	M4_IMASK	0xe4
#define	M4_ICLR		0xe8
#define	  M4_INT_RX		0x00000001
#define	  M4_INT_TX		0x00000002

/*
 * padre_gmac writes GO again whenever it sees STALL after queueing work, so
 * STALL is read as "stopped at a descriptor it does not own". Its close waits
 * for RX STALL to clear, which does not fit that reading; unconfirmed.
 */

/* MC DMA channel FIFO control; GMAC0 uses channels 0 (RX) and 1 (TX). */
#define	MC_DMA_FFCTRL(ch)	(0x60002800 + (ch) * 0x10)
#define	  MC_DMA_FFCTRL_EN	0x00008000

/* DJ: a reset pulse per GMAC, and GMT, the segmentation engine. */
#define	DJ_RESET	0x60005000
#define	  DJ_RESET_GM0		0x00000001
#define	DJ_CTRL		0x60005004
#define	  DJ_CTRL_GM0		0x00000020
#define	GMT0_CTRL	0x60005040

#define	GM0_IRQ		41	/* the M4 line */

/*
 * M4 descriptors, 16 bytes, linked through physical addresses. The owner
 * bit is set while the DMA engine owns the descriptor. The engine only
 * took them from S3 SRAM, where padre_gmac keeps them; from DDR it never
 * started.
 */
struct m4_desc {
	uint32_t	d_link;
	uint32_t	d_buf;
	uint32_t	d_ctrl;
	uint32_t	d_stat;	/* RX: bit 23 and the length with FCS */
};

#define	M4D_OWNER	0x80000000
#define	M4D_ERR		0x40000000
#define	M4D_SOP		0x20000000
#define	M4D_INTR1	0x10000000	/* GMAC1 */
#define	M4D_INTR0	0x08000000	/* GMAC0 */
#define	M4D_META	0x04000000
#define	M4D_EOP		0x02000000
#define	M4D_DIR		0x01000000
#define	M4D_SIZE	0x00ffffff

#ifdef _KERNEL
uint32_t gmac_macread(int);
void	gmac_macwrite(int, uint32_t);
#endif

#endif /* _PADRE_GMACREG_H_ */
