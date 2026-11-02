/*	$NetBSD$	*/

#ifndef _PADRE_TYPES_H_
#define _PADRE_TYPES_H_

#include <sparc/types.h>

#undef	__HAVE_CPU_VMSPACE_EXEC
#undef	__HAVE_RAS

#define	__HAVE_CPU_DATA_FIRST
#define	__HAVE_CPU_UAREA_ROUTINES
#define	__HAVE_VM_PAGE_MD

#endif /* _PADRE_TYPES_H_ */
