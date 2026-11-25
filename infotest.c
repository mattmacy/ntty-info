// SPDX-License-Identifier: GPL-2.0
/*
 * infotest - a tiny program to verify the n_tty_info module.
 *
 * It installs a handler for the chosen "info" signal (SIGUSR1 by default,
 * matching the module's default) and then counts up once a second. Press ^T
 * and it should print a status line without dying.
 *
 *   ./infotest            handle SIGUSR1
 *   ./infotest 28         handle SIGWINCH instead (if you loaded the module
 *                         with infosig=28)
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

static volatile sig_atomic_t counter;
static volatile sig_atomic_t pinged;

static void on_info(int sig)
{
	(void)sig;
	pinged = 1;	/* do real work outside the handler */
}

int main(int argc, char **argv)
{
	int sig = SIGUSR1;
	struct sigaction sa;

	if (argc > 1)
		sig = atoi(argv[1]);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_info;
	sa.sa_flags = SA_RESTART;	/* restart EINTR in our body; sleep() is
					 * still cut short by a handler, so we
					 * only bump the counter on a full second */
	if (sigaction(sig, &sa, NULL) < 0) {
		perror("sigaction");
		return 1;
	}

	printf("infotest: pid=%d, handling signal %d. Press ^T...\n",
	       (int)getpid(), sig);

	for (;;) {
		if (sleep(1) == 0)
			counter++;	/* only count uninterrupted full seconds */
		if (pinged) {
			pinged = 0;
			printf("\r[^T] infotest: %d seconds elapsed\n", counter);
			fflush(stdout);
		}
	}
	return 0;
}
