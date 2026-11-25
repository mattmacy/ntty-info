// SPDX-License-Identifier: GPL-2.0
/*
 * dddemo - demonstrate n_tty_info's ^T on the host WITHOUT touching your real
 * terminal. It creates a throwaway pty, runs `dd` as the foreground job inside
 * it, sets the n_tty_info line discipline on that pty, injects ^T a couple of
 * times, and prints whatever comes back (dd's SIGUSR1 status line + the
 * module's own on-tty status line). Everything is confined to this pty, so a
 * misbehaving ldisc cannot disturb your desktop session.
 *
 * Needs the module already loaded (sudo insmod n_tty_info.ko). No root needed
 * to run this if TIOCSETD on your own pty is permitted (it normally is).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>

#define N_TTY_INFO 29		/* N_DEVELOPMENT */

int main(void)
{
	int mfd, sfd, ptn, unlock = 0, ld = N_TTY_INFO;
	char path[64];
	pid_t pid;

	mfd = posix_openpt(O_RDWR | O_NOCTTY);
	if (mfd < 0) { perror("posix_openpt"); return 1; }
	ioctl(mfd, TIOCSPTLCK, &unlock);
	if (ioctl(mfd, TIOCGPTN, &ptn) < 0) { perror("TIOCGPTN"); return 1; }
	snprintf(path, sizeof(path), "/dev/pts/%d", ptn);
	sfd = open(path, O_RDWR | O_NOCTTY);
	if (sfd < 0) { perror(path); return 1; }

	pid = fork();
	if (pid < 0) { perror("fork"); return 1; }
	if (pid == 0) {			/* child: become fg job of the pty, run dd */
		close(mfd);
		setsid();
		if (ioctl(sfd, TIOCSCTTY, 0) < 0) { perror("TIOCSCTTY"); _exit(3); }
		if (ioctl(sfd, TIOCSETD, &ld) < 0) {
			fprintf(stderr, "TIOCSETD(n_tty_info): %s\n", strerror(errno));
			_exit(4);
		}
		dup2(sfd, 0); dup2(sfd, 1); dup2(sfd, 2);
		if (sfd > 2) close(sfd);
		/* steady, long-running dd so there is always something to report */
		execlp("dd", "dd", "if=/dev/zero", "of=/dev/null", "bs=1M", (char *)NULL);
		perror("execlp dd");
		_exit(5);
	}

	close(sfd);
	printf("=== n_tty_info host demo: dd running in pty %s, pid %d ===\n", path, pid);
	printf("    (injecting ^T twice; watch for dd's status + the module's 'cmd:' line)\n\n");
	fflush(stdout);

	struct pollfd pfd = { .fd = mfd, .events = POLLIN };
	int i;
	for (i = 0; i < 24; i++) {		/* ~3.6s total */
		if (i == 4 || i == 12) {
			const char t = 0x14;	/* ^T */
			printf("\n>>> [t=%.1fs] injecting ^T\n", i * 0.15);
			fflush(stdout);
			if (write(mfd, &t, 1) != 1) perror("write ^T");
		}
		if (poll(&pfd, 1, 150) > 0 && (pfd.revents & POLLIN)) {
			char buf[4096];
			int n = read(mfd, buf, sizeof(buf));
			if (n > 0) { fwrite(buf, 1, n, stdout); fflush(stdout); }
		}
	}

	printf("\n\n=== stopping dd ===\n");
	kill(pid, SIGTERM);
	/* drain dd's final "records in/out" summary */
	for (i = 0; i < 6; i++) {
		if (poll(&pfd, 1, 150) > 0 && (pfd.revents & POLLIN)) {
			char buf[4096];
			int n = read(mfd, buf, sizeof(buf));
			if (n > 0) { fwrite(buf, 1, n, stdout); fflush(stdout); }
		}
	}
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	close(mfd);
	printf("=== done ===\n");
	return 0;
}
