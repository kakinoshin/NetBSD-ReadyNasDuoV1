/*	$NetBSD$	*/

/*
 * Bus space for the IT3107: a handle is the CPU address of the register, see
 * <machine/bus_defs.h>. Plain accesses, as the bridge swaps the byte lanes.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>

int
bus_space_map(bus_space_tag_t t, bus_addr_t a, bus_size_t s, int f,
    bus_space_handle_t *hp)
{

	*hp = t->pbs_base + a;
	return 0;
}

void
bus_space_unmap(bus_space_tag_t t, bus_space_handle_t h, bus_size_t s)
{
}

int
bus_space_subregion(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o,
    bus_size_t s, bus_space_handle_t *hp)
{

	*hp = h + o;
	return 0;
}

void *
bus_space_vaddr(bus_space_tag_t t, bus_space_handle_t h)
{

	return (void *)h;
}

paddr_t
bus_space_mmap(bus_space_tag_t t, bus_addr_t a, off_t o, int p, int f)
{

	return -1;
}

void
bus_space_barrier(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o,
    bus_size_t s, int f)
{

	__asm volatile("" ::: "memory");
}

#define	__PADRE_BS_RW(n, T)						\
T							\
bus_space_read_##n(bus_space_tag_t t, bus_space_handle_t h,		\
    bus_size_t o)							\
{									\
	return *(volatile T *)(h + o);					\
}									\
void							\
bus_space_write_##n(bus_space_tag_t t, bus_space_handle_t h,		\
    bus_size_t o, T v)							\
{									\
	*(volatile T *)(h + o) = v;					\
}									\
void							\
bus_space_read_multi_##n(bus_space_tag_t t, bus_space_handle_t h,	\
    bus_size_t o, T *a, bus_size_t c)					\
{									\
	while (c-- > 0)							\
		*a++ = *(volatile T *)(h + o);				\
}									\
void							\
bus_space_write_multi_##n(bus_space_tag_t t, bus_space_handle_t h,	\
    bus_size_t o, const T *a, bus_size_t c)				\
{									\
	while (c-- > 0)							\
		*(volatile T *)(h + o) = *a++;				\
}									\
void							\
bus_space_read_region_##n(bus_space_tag_t t, bus_space_handle_t h,	\
    bus_size_t o, T *a, bus_size_t c)					\
{									\
	for (; c-- > 0; o += sizeof(T))					\
		*a++ = *(volatile T *)(h + o);				\
}									\
void							\
bus_space_write_region_##n(bus_space_tag_t t, bus_space_handle_t h,	\
    bus_size_t o, const T *a, bus_size_t c)				\
{									\
	for (; c-- > 0; o += sizeof(T))					\
		*(volatile T *)(h + o) = *a++;				\
}									\
void							\
bus_space_set_multi_##n(bus_space_tag_t t, bus_space_handle_t h,	\
    bus_size_t o, T v, bus_size_t c)					\
{									\
	while (c-- > 0)							\
		*(volatile T *)(h + o) = v;				\
}									\
void							\
bus_space_set_region_##n(bus_space_tag_t t, bus_space_handle_t h,	\
    bus_size_t o, T v, bus_size_t c)					\
{									\
	for (; c-- > 0; o += sizeof(T))					\
		*(volatile T *)(h + o) = v;				\
}									\
void							\
bus_space_copy_region_##n(bus_space_tag_t t, bus_space_handle_t h1,	\
    bus_size_t o1, bus_space_handle_t h2, bus_size_t o2, bus_size_t c)	\
{									\
	for (; c-- > 0; o1 += sizeof(T), o2 += sizeof(T))		\
		*(volatile T *)(h2 + o2) = *(volatile T *)(h1 + o1);	\
}

__PADRE_BS_RW(1, uint8_t)
__PADRE_BS_RW(2, uint16_t)
__PADRE_BS_RW(4, uint32_t)

int
bus_dmatag_subregion(bus_dma_tag_t t, bus_addr_t min, bus_addr_t max,
    bus_dma_tag_t *nt, int f)
{

	return EOPNOTSUPP;
}

void
bus_dmatag_destroy(bus_dma_tag_t t)
{
}

