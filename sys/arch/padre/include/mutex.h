/*	$NetBSD$	*/

#ifndef _PADRE_MUTEX_H_
#define _PADRE_MUTEX_H_

#include <sys/types.h>

#ifdef __MUTEX_PRIVATE
#include <machine/intr.h>
#include <machine/psl.h>
#endif

struct kmutex {
	union {
		volatile uintptr_t	mtxa_owner;
#ifdef __MUTEX_PRIVATE
		struct {
			volatile uint8_t	mtxs_dummy;
			ipl_cookie_t		mtxs_ipl;
			__cpu_simple_lock_t	mtxs_lock;
			volatile uint8_t	mtxs_unused;
		} s;
#endif
	} u;
};

#ifdef __MUTEX_PRIVATE

#define	mtx_owner 			u.mtxa_owner
#define	mtx_ipl 			u.s.mtxs_ipl
#define	mtx_lock			u.s.mtxs_lock

#define	__HAVE_SIMPLE_MUTEXES		1

#define	MUTEX_CAS(p, o, n)		\
    (_atomic_cas_ulong((volatile unsigned long *)(p), (o), (n)) == (o))

unsigned long	_atomic_cas_ulong(volatile unsigned long *,
    unsigned long, unsigned long);

#endif	/* __MUTEX_PRIVATE */

#endif /* _PADRE_MUTEX_H_ */
