/*	$NetBSD$	*/

/*
 * bus_dma for the IT3107. Bus addresses are physical addresses, and PCI DMA
 * does not snoop the write-back D-cache, so syncs clean or invalidate lines by
 * address through the DCMU probe modes, as Linux's padre_cache.c does.
 * BUS_DMA_COHERENT memory is mapped non-cached.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/cpu.h>
#include <sys/kmem.h>
#include <sys/mbuf.h>
#include <sys/proc.h>
#include <sys/uio.h>

#include <uvm/uvm.h>

#define	ASI_DCMU		0x15
#define	DCMU_CTRL		0x00030000
#define	DCMU_PROBE_CLEAN	2
#define	DCMU_PROBE_INVCLN	3
#define	DCACHE_LINE		16
#define	RAM_PA_END		0x7f000000UL

const struct padre_bus_dma_tag padre_bus_dma_tag;

/* Applies a probe mode to every line of [pa, pa + len) through the direct map. */
static void
dcache_probe(paddr_t pa, bus_size_t len, u_int mode)
{
	vaddr_t va = PMAP_DIRECT_VA(pa) & ~(DCACHE_LINE - 1);
	const vaddr_t end = PMAP_DIRECT_VA(pa) + len;
	const u_int psr = getpsr();
	uint32_t ctrl;

	if (len == 0 || pa >= RAM_PA_END)
		return;
	setpsr(psr | PSR_PIL);
	__asm volatile("lda [%1] %2, %0" : "=r" (ctrl) :
	    "r" (DCMU_CTRL), "n" (ASI_DCMU));
	__asm volatile("sta %0, [%1] %2" : :
	    "r" (ctrl | (mode << 4)), "r" (DCMU_CTRL), "n" (ASI_DCMU));
	for (; va < end; va += DCACHE_LINE)
		__asm volatile("ld [%0], %%g0" : : "r" (va) : "memory");
	__asm volatile("sta %0, [%1] %2" : :
	    "r" (ctrl), "r" (DCMU_CTRL), "n" (ASI_DCMU));
	setpsr(psr);
}

int
bus_dmamap_create(bus_dma_tag_t t, bus_size_t size, int nsegments,
    bus_size_t maxsegsz, bus_size_t boundary, int flags, bus_dmamap_t *dmamp)
{
	const size_t mapsize = sizeof(struct padre_bus_dmamap) +
	    sizeof(bus_dma_segment_t) * (nsegments - 1);
	bus_dmamap_t map;

	map = kmem_zalloc(mapsize, (flags & BUS_DMA_NOWAIT) ?
	    KM_NOSLEEP : KM_SLEEP);
	if (map == NULL)
		return ENOMEM;
	map->_dm_size = size;
	map->_dm_segcnt = nsegments;
	map->_dm_maxsegsz = maxsegsz;
	map->_dm_boundary = boundary;
	map->_dm_flags = flags;
	map->dm_maxsegsz = maxsegsz;
	*dmamp = map;
	return 0;
}

void
bus_dmamap_destroy(bus_dma_tag_t t, bus_dmamap_t map)
{

	kmem_free(map, sizeof(struct padre_bus_dmamap) +
	    sizeof(bus_dma_segment_t) * (map->_dm_segcnt - 1));
}

/* Appends [pa, pa + len) to the map, merging with the last segment if it can. */
static int
dmamap_add(bus_dmamap_t map, paddr_t pa, bus_size_t len)
{
	const bus_addr_t bmask = ~(map->_dm_boundary - 1);
	bus_dma_segment_t *ds;
	bus_size_t sgsize;

	while (len > 0) {
		sgsize = len;
		if (map->_dm_boundary > 0) {
			const bus_addr_t next = (pa + map->_dm_boundary) & bmask;

			if (sgsize > next - pa)
				sgsize = next - pa;
		}
		ds = &map->dm_segs[map->dm_nsegs - 1];
		if (map->dm_nsegs > 0 && ds->ds_addr + ds->ds_len == pa &&
		    ds->ds_len + sgsize <= map->dm_maxsegsz &&
		    (map->_dm_boundary == 0 ||
		    (ds->ds_addr & bmask) == (pa & bmask))) {
			ds->ds_len += sgsize;
		} else {
			if (map->dm_nsegs == map->_dm_segcnt)
				return EFBIG;
			ds = &map->dm_segs[map->dm_nsegs++];
			ds->ds_addr = pa;
			ds->ds_len = sgsize;
		}
		pa += sgsize;
		len -= sgsize;
	}
	return 0;
}

static int
dmamap_load_buffer(bus_dmamap_t map, void *buf, bus_size_t buflen,
    pmap_t pmap)
{
	vaddr_t va = (vaddr_t)buf;
	bus_size_t sgsize;
	paddr_t pa;
	int error;

	while (buflen > 0) {
		if (!pmap_extract(pmap, va, &pa))
			return EFAULT;
		sgsize = PAGE_SIZE - (va & PAGE_MASK);
		if (sgsize > buflen)
			sgsize = buflen;
		error = dmamap_add(map, pa, sgsize);
		if (error != 0)
			return error;
		va += sgsize;
		buflen -= sgsize;
	}
	return 0;
}

int
bus_dmamap_load(bus_dma_tag_t t, bus_dmamap_t map, void *buf,
    bus_size_t buflen, struct proc *p, int flags)
{
	int error;

	map->dm_mapsize = 0;
	map->dm_nsegs = 0;
	if (buflen > map->_dm_size)
		return EINVAL;
	error = dmamap_load_buffer(map, buf, buflen, p != NULL ?
	    vm_map_pmap(&p->p_vmspace->vm_map) : pmap_kernel());
	if (error != 0) {
		map->dm_nsegs = 0;
		return error;
	}
	map->dm_mapsize = buflen;
	return 0;
}

int
bus_dmamap_load_mbuf(bus_dma_tag_t t, bus_dmamap_t map, struct mbuf *m0,
    int flags)
{
	struct mbuf *m;
	int error;

	map->dm_mapsize = 0;
	map->dm_nsegs = 0;
	if (m0->m_pkthdr.len > map->_dm_size)
		return EINVAL;
	for (m = m0; m != NULL; m = m->m_next) {
		if (m->m_len == 0)
			continue;
		error = dmamap_load_buffer(map, m->m_data, m->m_len,
		    pmap_kernel());
		if (error != 0) {
			map->dm_nsegs = 0;
			return error;
		}
	}
	map->dm_mapsize = m0->m_pkthdr.len;
	return 0;
}

int
bus_dmamap_load_uio(bus_dma_tag_t t, bus_dmamap_t map, struct uio *uio,
    int flags)
{
	const pmap_t pmap = VMSPACE_IS_KERNEL_P(uio->uio_vmspace) ?
	    pmap_kernel() : vm_map_pmap(&uio->uio_vmspace->vm_map);
	bus_size_t resid = uio->uio_resid, len;
	int error, i;

	map->dm_mapsize = 0;
	map->dm_nsegs = 0;
	if (resid > map->_dm_size)
		return EINVAL;
	for (i = 0; i < uio->uio_iovcnt && resid > 0; i++) {
		len = MIN(resid, uio->uio_iov[i].iov_len);
		error = dmamap_load_buffer(map, uio->uio_iov[i].iov_base, len,
		    pmap);
		if (error != 0) {
			map->dm_nsegs = 0;
			return error;
		}
		resid -= len;
	}
	map->dm_mapsize = uio->uio_resid;
	return 0;
}

int
bus_dmamap_load_raw(bus_dma_tag_t t, bus_dmamap_t map,
    bus_dma_segment_t *segs, int nsegs, bus_size_t size, int flags)
{
	bus_size_t left = size;
	int error, i;

	map->dm_mapsize = 0;
	map->dm_nsegs = 0;
	for (i = 0; i < nsegs && left > 0; i++) {
		const bus_size_t len = MIN(left, segs[i].ds_len);

		error = dmamap_add(map, segs[i].ds_addr, len);
		if (error != 0) {
			map->dm_nsegs = 0;
			return error;
		}
		left -= len;
	}
	map->dm_mapsize = size;
	return 0;
}

void
bus_dmamap_unload(bus_dma_tag_t t, bus_dmamap_t map)
{

	map->dm_mapsize = 0;
	map->dm_nsegs = 0;
}

/*
 * Before the device writes, dirty lines are cleaned and dropped so that no
 * eviction lands on top of the DMA, and again afterwards so that the CPU reads
 * memory. Before the device reads, dirty lines are cleaned.
 */
void
bus_dmamap_sync(bus_dma_tag_t t, bus_dmamap_t map, bus_addr_t offset,
    bus_size_t len, int ops)
{
	u_int mode;
	int i;

	if (ops & (BUS_DMASYNC_PREREAD | BUS_DMASYNC_POSTREAD))
		mode = DCMU_PROBE_INVCLN;
	else if (ops & BUS_DMASYNC_PREWRITE)
		mode = DCMU_PROBE_CLEAN;
	else
		return;
	for (i = 0; i < map->dm_nsegs && len > 0; i++) {
		const bus_dma_segment_t * const ds = &map->dm_segs[i];
		bus_size_t l;

		if (offset >= ds->ds_len) {
			offset -= ds->ds_len;
			continue;
		}
		l = MIN(len, ds->ds_len - offset);
		dcache_probe(ds->ds_addr + offset, l, mode);
		offset = 0;
		len -= l;
	}
}

int
bus_dmamem_alloc(bus_dma_tag_t t, bus_size_t size, bus_size_t alignment,
    bus_size_t boundary, bus_dma_segment_t *segs, int nsegs, int *rsegs,
    int flags)
{
	struct pglist mlist;
	struct vm_page *m;
	paddr_t curaddr, lastaddr;
	int curseg, error;

	size = round_page(size);
	error = uvm_pglistalloc(size, 0, RAM_PA_END - 1, alignment, boundary,
	    &mlist, nsegs, (flags & BUS_DMA_NOWAIT) == 0);
	if (error != 0)
		return error;

	m = TAILQ_FIRST(&mlist);
	curseg = 0;
	lastaddr = segs[0].ds_addr = VM_PAGE_TO_PHYS(m);
	segs[0].ds_len = PAGE_SIZE;
	while ((m = TAILQ_NEXT(m, pageq.queue)) != NULL) {
		curaddr = VM_PAGE_TO_PHYS(m);
		if (curaddr == lastaddr + PAGE_SIZE)
			segs[curseg].ds_len += PAGE_SIZE;
		else {
			curseg++;
			segs[curseg].ds_addr = curaddr;
			segs[curseg].ds_len = PAGE_SIZE;
		}
		lastaddr = curaddr;
	}
	*rsegs = curseg + 1;
	return 0;
}

void
bus_dmamem_free(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs)
{
	paddr_t pa;
	int i;

	for (i = 0; i < nsegs; i++) {
		for (pa = segs[i].ds_addr; pa < segs[i].ds_addr + segs[i].ds_len;
		    pa += PAGE_SIZE)
			uvm_pagefree(PHYS_TO_VM_PAGE(pa));
	}
}

int
bus_dmamem_map(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs,
    size_t size, void **kvap, int flags)
{
	const bool nocache = (flags & (BUS_DMA_COHERENT | BUS_DMA_NOCACHE)) != 0;
	vaddr_t va;
	paddr_t pa;
	int i;

	size = round_page(size);
	if (nsegs == 1 && !nocache) {
		*kvap = (void *)PMAP_DIRECT_VA(segs[0].ds_addr);
		return 0;
	}
	va = uvm_km_alloc(kernel_map, size, 0, UVM_KMF_VAONLY |
	    ((flags & BUS_DMA_NOWAIT) ? UVM_KMF_NOWAIT : UVM_KMF_WAITVA));
	if (va == 0)
		return ENOMEM;
	*kvap = (void *)va;
	for (i = 0; i < nsegs; i++) {
		/* No dirty line of the direct map may land on the device's data. */
		if (nocache)
			dcache_probe(segs[i].ds_addr, segs[i].ds_len,
			    DCMU_PROBE_INVCLN);
		for (pa = segs[i].ds_addr;
		    pa < segs[i].ds_addr + segs[i].ds_len && size > 0;
		    pa += PAGE_SIZE, va += PAGE_SIZE, size -= PAGE_SIZE)
			pmap_kenter_pa(va, pa, VM_PROT_READ | VM_PROT_WRITE,
			    nocache ? PMAP_NOCACHE : 0);
	}
	pmap_update(pmap_kernel());
	return 0;
}

void
bus_dmamem_unmap(bus_dma_tag_t t, void *kva, size_t size)
{
	const vaddr_t va = (vaddr_t)kva;

	if (va < VM_MIN_KERNEL_ADDRESS)
		return;
	size = round_page(size);
	pmap_kremove(va, size);
	pmap_update(pmap_kernel());
	uvm_km_free(kernel_map, va, size, UVM_KMF_VAONLY);
}

paddr_t
bus_dmamem_mmap(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs,
    off_t off, int prot, int flags)
{
	int i;

	for (i = 0; i < nsegs; i++) {
		if (off < segs[i].ds_len)
			return atop(segs[i].ds_addr + off);
		off -= segs[i].ds_len;
	}
	return -1;
}
