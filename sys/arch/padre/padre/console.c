/*	$NetBSD$	*/

/*
 * Polled console on UART0, which is the connector on the board. iboot has
 * already set it to 9600 8N1.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>

#include <dev/cons.h>

#define	UART_DATA	(*(volatile uint32_t *)0x60000060)
#define	UART_STAT	(*(volatile uint32_t *)0x60000064)
#define	UART_STAT_DR	0x1
#define	UART_STAT_TH	0x4

static int
padre_cngetc(dev_t dev)
{

	while ((UART_STAT & UART_STAT_DR) == 0)
		continue;
	return UART_DATA & 0xff;
}

static void
padre_cnputc(dev_t dev, int c)
{

	while ((UART_STAT & UART_STAT_TH) == 0)
		continue;
	UART_DATA = c;
}

static void
padre_cnpollc(dev_t dev, int on)
{
}

static struct consdev padre_cons = {
	.cn_getc = padre_cngetc,
	.cn_putc = padre_cnputc,
	.cn_pollc = padre_cnpollc,
	.cn_dev = NODEV,
	.cn_pri = CN_NORMAL,
};

void
consinit(void)
{

	cn_tab = &padre_cons;
}
