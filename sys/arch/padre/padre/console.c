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

#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/intr.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <sys/tty.h>

#include <dev/cons.h>

#define	UART_DATA	(*(volatile uint32_t *)0x60000060)
#define	UART_STAT	(*(volatile uint32_t *)0x60000064)
#define	UART_CTRL	(*(volatile uint32_t *)0x60000068)
#define	UART_CTRL_RI	0x4
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

/* From majors.padre. */
#define	PCONS_MAJOR	4

void	pconsattach(int);

static struct tty *pcons_tty;

static int	pcons_intr(void *);
static void	pcons_start(struct tty *);
static int	pcons_param(struct tty *, struct termios *);

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

	padre_cons.cn_dev = makedev(PCONS_MAJOR, 0);
	cn_tab = &padre_cons;
}

/*
 * The same UART as a tty. Output is polled; input takes the receive interrupt
 * because the UART holds a single character.
 */
void
pconsattach(int n)
{

	pcons_tty = tty_alloc();
	tty_attach(pcons_tty);
	pcons_tty->t_oproc = pcons_start;
	pcons_tty->t_param = pcons_param;
	UART_STAT = 0xff;
	padre_intr_establish(IRQ_UART0, IPL_TTY, pcons_intr, pcons_tty);
	UART_CTRL |= UART_CTRL_RI;
}

static int
pcons_param(struct tty *tp, struct termios *t)
{

	tp->t_ispeed = t->c_ispeed;
	tp->t_ospeed = t->c_ospeed;
	tp->t_cflag = t->c_cflag;
	return 0;
}

static void
pcons_start(struct tty *tp)
{
	const int s = spltty();
	struct clist * const cl = &tp->t_outq;

	if ((tp->t_state & (TS_BUSY | TS_TIMEOUT | TS_TTSTOP)) == 0) {
		while (cl->c_cc > 0)
			padre_cnputc(NODEV, getc(cl));
		ttypull(tp);
	}
	splx(s);
}

static int
pcons_intr(void *arg)
{
	struct tty * const tp = arg;
	int n;

	/* Writing the status register clears its overrun and error flags. */
	for (n = 0; n < 256 && (UART_STAT & UART_STAT_DR) != 0; n++) {
		const int c = UART_DATA & 0xff;

		UART_STAT = 0xff;
		if (tp->t_state & TS_ISOPEN)
			(*tp->t_linesw->l_rint)(c, tp);
	}
	return 1;
}

static int
pcons_open(dev_t dev, int flag, int mode, struct lwp *l)
{
	struct tty * const tp = pcons_tty;
	int error;

	if (kauth_authorize_device_tty(l->l_cred, KAUTH_DEVICE_TTY_OPEN, tp))
		return EBUSY;
	mutex_spin_enter(&tty_lock);
	tp->t_dev = dev;
	if ((tp->t_state & TS_ISOPEN) == 0) {
		ttychars(tp);
		tp->t_iflag = TTYDEF_IFLAG;
		tp->t_oflag = TTYDEF_OFLAG;
		tp->t_lflag = TTYDEF_LFLAG;
		tp->t_cflag = TTYDEF_CFLAG | CLOCAL;
		tp->t_ispeed = tp->t_ospeed = 9600;
		ttsetwater(tp);
		tp->t_state |= TS_CARR_ON;
	}
	mutex_spin_exit(&tty_lock);
	error = ttyopen(tp, 0, flag & O_NONBLOCK);
	if (error == 0)
		error = (*tp->t_linesw->l_open)(dev, tp);
	return error;
}

static int
pcons_close(dev_t dev, int flag, int mode, struct lwp *l)
{
	struct tty * const tp = pcons_tty;

	(*tp->t_linesw->l_close)(tp, flag);
	return ttyclose(tp);
}

static int
pcons_read(dev_t dev, struct uio *uio, int flag)
{

	return (*pcons_tty->t_linesw->l_read)(pcons_tty, uio, flag);
}

static int
pcons_write(dev_t dev, struct uio *uio, int flag)
{

	return (*pcons_tty->t_linesw->l_write)(pcons_tty, uio, flag);
}

static int
pcons_poll_dev(dev_t dev, int events, struct lwp *l)
{

	return (*pcons_tty->t_linesw->l_poll)(pcons_tty, events, l);
}

static int
pcons_ioctl(dev_t dev, u_long cmd, void *data, int flag, struct lwp *l)
{
	struct tty * const tp = pcons_tty;
	int error;

	error = (*tp->t_linesw->l_ioctl)(tp, cmd, data, flag, l);
	if (error != EPASSTHROUGH)
		return error;
	return ttioctl(tp, cmd, data, flag, l);
}

static struct tty *
pcons_tp(dev_t dev)
{

	return pcons_tty;
}

const struct cdevsw pcons_cdevsw = {
	.d_open = pcons_open,
	.d_close = pcons_close,
	.d_read = pcons_read,
	.d_write = pcons_write,
	.d_ioctl = pcons_ioctl,
	.d_stop = nullstop,
	.d_tty = pcons_tp,
	.d_poll = pcons_poll_dev,
	.d_mmap = nommap,
	.d_kqfilter = ttykqfilter,
	.d_discard = nodiscard,
	.d_flag = D_TTY,
};
