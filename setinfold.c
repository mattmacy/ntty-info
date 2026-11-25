// SPDX-License-Identifier: GPL-2.0
/*
 * setinfold - attach (or remove) the n_tty_info line discipline on a tty.
 *
 * The line discipline is per-tty, not per-process: once set on your shell's
 * terminal, every program you run there inherits it until you restore N_TTY
 * or the terminal goes away.
 *
 *   setinfold            set n_tty_info on the controlling terminal
 *   setinfold -r         restore the standard N_TTY discipline
 *   setinfold /dev/pts/3 set it on a specific tty
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>

#ifndef N_TTY_INFO
#define N_TTY_INFO 29
#endif
#ifndef N_TTY
#define N_TTY 0
#endif

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s [-r] [tty]\n"
		"  Attach the n_tty_info line discipline to a tty.\n"
		"  Default tty is the controlling terminal (/dev/tty).\n"
		"    -r   restore the standard N_TTY discipline\n", p);
}

int main(int argc, char **argv)
{
	int ld = N_TTY_INFO, fd, opt;
	const char *dev = "/dev/tty";

	while ((opt = getopt(argc, argv, "rh")) != -1) {
		switch (opt) {
		case 'r': ld = N_TTY; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 2;
		}
	}
	if (optind < argc)
		dev = argv[optind];

	fd = open(dev, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		perror(dev);
		return 1;
	}

	if (!isatty(fd)) {
		fprintf(stderr, "%s: not a tty\n", dev);
		close(fd);
		return 1;
	}

	if (ioctl(fd, TIOCSETD, &ld) < 0) {
		perror("TIOCSETD");
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}
