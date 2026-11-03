/*	$NetBSD$	*/

/*
 * Bus space and DMA for the IT3107. The PCI bridge swaps byte lanes
 * (PCI_SWAP 0x4444), so device registers read in host order with plain
 * accesses, and the stream variants are the same. DMA memory keeps PCI byte
 * order, which drivers handle with htole32() and friends.
 */

#ifndef _PADRE_BUS_DEFS_H_
#define _PADRE_BUS_DEFS_H_

#include <sys/types.h>

typedef u_long	bus_addr_t;
typedef u_long	bus_size_t;

#define	PRIxBUSADDR	"lx"
#define	PRIxBUSSIZE	"lx"
#define	PRIuBUSSIZE	"lu"

/* A tag is the CPU address at which its bus starts. */
typedef const struct padre_bus_space *bus_space_tag_t;
typedef vaddr_t	bus_space_handle_t;

#define	PRIxBSH		"lx"

struct padre_bus_space {
	vaddr_t		pbs_base;
};

#define	__BUS_SPACE_HAS_STREAM_METHODS	1

#define	BUS_SPACE_MAP_CACHEABLE		0x01
#define	BUS_SPACE_MAP_LINEAR		0x02
#define	BUS_SPACE_MAP_PREFETCHABLE	0x04

#define	BUS_SPACE_BARRIER_READ		0x01
#define	BUS_SPACE_BARRIER_WRITE		0x02

#define	BUS_DMA_WAITOK		0x000
#define	BUS_DMA_NOWAIT		0x001
#define	BUS_DMA_ALLOCNOW	0x002
#define	BUS_DMA_COHERENT	0x004
#define	BUS_DMA_STREAMING	0x008
#define	BUS_DMA_BUS1		0x010
#define	BUS_DMA_BUS2		0x020
#define	BUS_DMA_BUS3		0x040
#define	BUS_DMA_BUS4		0x080
#define	BUS_DMA_READ		0x100
#define	BUS_DMA_WRITE		0x200
#define	BUS_DMA_NOCACHE		0x400
#define	BUS_DMA_PREFETCHABLE	0x800

#define	BUS_DMASYNC_PREREAD	0x01
#define	BUS_DMASYNC_POSTREAD	0x02
#define	BUS_DMASYNC_PREWRITE	0x04
#define	BUS_DMASYNC_POSTWRITE	0x08

struct mbuf;
struct uio;

/* Bus addresses are physical addresses. */
typedef struct padre_bus_dma_segment {
	bus_addr_t	ds_addr;
	bus_size_t	ds_len;
} bus_dma_segment_t;

typedef const struct padre_bus_dma_tag *bus_dma_tag_t;

struct padre_bus_dma_tag {
	int		_dummy;
};

typedef struct padre_bus_dmamap {
	bus_size_t	_dm_size;
	int		_dm_segcnt;
	bus_size_t	_dm_maxsegsz;
	bus_size_t	_dm_boundary;
	int		_dm_flags;

	bus_size_t	dm_maxsegsz;
	bus_size_t	dm_mapsize;
	int		dm_nsegs;
	bus_dma_segment_t dm_segs[1];
} *bus_dmamap_t;

#endif /* _PADRE_BUS_DEFS_H_ */
