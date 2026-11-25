// SPDX-License-Identifier: GPL-2.0
/*
 * init.c - PID 1 for a throwaway initramfs that tests the n_tty_info module.
 *
 * Mounts the pseudo filesystems, loads n_tty_info.ko, runs a matrix of
 * pty-based tests (from the review checklist), prints a single PASS/FAIL
 * sentinel, and powers off. Every test runs in a forked child so a failure
 * never crashes PID 1. Built -static, placed at /init in the cpio.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <termios.h>

#define N_TTY_INFO 29		/* N_DEVELOPMENT */
#ifndef N_TTY
#define N_TTY 0
#endif
#define ST 0x14			/* ^T */

static int con = 2;
static int passes, fails;

static void klog(const char *fmt, ...)
{
	char b[512]; va_list ap; int n;
	va_start(ap, fmt); n = vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
	if (n > 0) write(con, b, n);
}

static int finit_module(int fd, const char *a, int f) { return syscall(SYS_finit_module, fd, a, f); }
static int delete_module(const char *n, int f) { return syscall(SYS_delete_module, n, f); }

static int load_mod(const char *args)
{
	int fd = open("/n_tty_info.ko", O_RDONLY), r;
	if (fd < 0) { klog("  open ko: %s\n", strerror(errno)); return -1; }
	r = finit_module(fd, args, 0); close(fd);
	if (r < 0) { klog("  finit_module(%s): %s\n", args, strerror(errno)); return -1; }
	return 0;
}

static int open_pty(int *m, int *s)
{
	int mfd, sfd, ptn, unlock = 0; char path[64];
	mfd = open("/dev/ptmx", O_RDWR | O_NOCTTY);
	if (mfd < 0) { klog("  ptmx: %s\n", strerror(errno)); return -1; }
	if (ioctl(mfd, TIOCSPTLCK, &unlock) < 0 || ioctl(mfd, TIOCGPTN, &ptn) < 0) {
		klog("  ptmx setup: %s\n", strerror(errno)); close(mfd); return -1; }
	snprintf(path, sizeof(path), "/dev/pts/%d", ptn);
	sfd = open(path, O_RDWR | O_NOCTTY);
	if (sfd < 0) { klog("  open %s: %s\n", path, strerror(errno)); close(mfd); return -1; }
	*m = mfd; *s = sfd; return 0;
}

/* read until substr seen or idle timeout; returns total bytes in buf */
static int drain(int fd, char *buf, int cap, int ms, const char *stop)
{
	int tot = 0; struct pollfd pfd = { .fd = fd, .events = POLLIN };
	for (;;) {
		int r = poll(&pfd, 1, ms);
		if (r <= 0) break;
		if (!(pfd.revents & POLLIN)) break;
		int n = read(fd, buf + tot, cap - 1 - tot);
		if (n <= 0) break;
		tot += n; buf[tot] = 0;
		if (tot >= cap - 1) break;
		if (stop) { int ln = strlen(stop), i;
			for (i = 0; i + ln <= tot; i++) if (!memcmp(buf + i, stop, ln)) return tot; }
	}
	return tot;
}

static int memfind(const char *h, int n, const char *s)
{
	int ln = strlen(s), i; for (i = 0; i + ln <= n; i++) if (!memcmp(h + i, s, ln)) return i; return -1;
}
static int count_sub(const char *h, int n, const char *s)
{
	int ln = strlen(s), i, c = 0; for (i = 0; i + ln <= n; i++) if (!memcmp(h + i, s, ln)) { c++; i += ln - 1; } return c;
}
static int has_byte(const char *h, int n, char b) { int i; for (i = 0; i < n; i++) if (h[i] == b) return 1; return 0; }

/* ---- child ------------------------------------------------------------- */
struct cfg {
	int ldisc, canon, isig, iexten, echo, raw;
	int install_handler, handler_sig, read_n, delay_ms, block_pipe;
};
static struct cfg C;
static void handler(int s) { (void)s; write(1, "SIGHIT", 6); }

static void child_main(int sfd)
{
	struct termios t;
	alarm(8);		/* safety net: never let a stuck child hang the parent's waitpid */
	setsid();
	if (ioctl(sfd, TIOCSCTTY, 0) < 0) { write(2, "ERRCTTY", 7); _exit(3); }
	if (ioctl(sfd, TIOCSETD, &C.ldisc) < 0) { write(2, "ERRSETD", 7); _exit(4); }
	/* route child stdio to the slave pty so all I/O goes through the ldisc */
	dup2(sfd, 0); dup2(sfd, 1); dup2(sfd, 2);
	if (sfd > 2) close(sfd);
	tcgetattr(0, &t);
	if (C.raw) cfmakeraw(&t);
	else {
		t.c_lflag &= ~(ICANON | ISIG | IEXTEN | ECHO);
		if (C.canon) t.c_lflag |= ICANON;
		if (C.isig) t.c_lflag |= ISIG;
		if (C.iexten) t.c_lflag |= IEXTEN;
		if (C.echo) t.c_lflag |= ECHO;
	}
	tcsetattr(0, TCSANOW, &t);
	if (C.install_handler) {
		struct sigaction sa; memset(&sa, 0, sizeof(sa));
		sa.sa_handler = handler; sa.sa_flags = SA_RESTART;
		sigaction(C.handler_sig, &sa, NULL);
	}
	write(1, "RDY", 3);
	if (C.block_pipe) {		/* sleep in pipe_read -> a stable, real wait channel */
		int pp[2]; char c;
		if (pipe(pp) == 0) read(pp[0], &c, 1);
		for (;;) pause();
	}
	if (C.read_n > 0) {
		static char rb[9000]; int tot = 0, allx = 1, i;
		if (C.delay_ms) usleep(C.delay_ms * 1000);
		while (tot < C.read_n) { int n = read(0, rb + tot, C.read_n - tot); if (n <= 0) break; tot += n; }
		for (i = 0; i < tot; i++) if (rb[i] != 'x') allx = 0;
		char out[80]; int k;
		if (C.read_n <= 16) k = snprintf(out, sizeof(out), "DRN[%.*s]", tot, rb);
		else k = snprintf(out, sizeof(out), "DRN:%d:%s", tot, allx ? "allx" : "BAD");
		write(1, out, k);
		_exit(0);
	}
	for (;;) pause();
}

static pid_t start(int *mp)
{
	int m, s; pid_t pid;
	if (open_pty(&m, &s) < 0) return -1;
	pid = fork();
	if (pid == 0) { close(m); child_main(s); _exit(9); }
	close(s); *mp = m; return pid;
}

static int wait_rdy(int m);	/* defined after main */

static void record(const char *name, int ok, const char *d)
{
	if (ok) { passes++; klog("  [PASS] %s\n", name); }
	else { fails++; klog("  [FAIL] %s -- %s\n", name, d ? d : ""); }
}

int main(void)
{
	char b[16384]; int n, alive; pid_t pid; int m, s, st, r1, r2, e1;

	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	mkdir("/dev/pts", 0755);
	mount("devpts", "/dev/pts", "devpts", 0, "ptmxmode=0666");
	con = open("/dev/console", O_RDWR); if (con < 0) con = 2;

	klog("\n==== n_tty_info VM test ====\n");

	/* ---------------- cycle A: infosig=28 (SIGWINCH) ---------------- */
	if (load_mod("infosig=28 ttystatus=1 logstatus=1") < 0) { klog("RESULT: FAIL\n"); goto done; }
	klog("loaded infosig=28\n");

	/* T1 control: N_TTY swallows nothing, signals nothing */
	C = (struct cfg){ N_TTY,1,1,1,1,0, 1,28,0,0 };
	if ((pid = start(&m)) > 0) {
		if (wait_rdy(m)) { write(m, "a\x14""b\n", 4); n = drain(m, b, sizeof(b), 700, NULL);
			record("T1 control (N_TTY: no fire)",
			       count_sub(b,n,"cmd:")==0 && memfind(b,n,"SIGHIT")<0, "unexpected fire under N_TTY"); }
		else record("T1 control", 0, "no RDY");
		kill(pid,SIGKILL); waitpid(pid,&st,0); close(m);
	} else record("T1 control", 0, "start failed");

	/* T2 cooked: fire + splice + status line + signal */
	C = (struct cfg){ N_TTY_INFO,1,1,1,1,0, 1,28,2,0 };
	if ((pid = start(&m)) > 0) {
		if (wait_rdy(m)) { write(m, "a\x14""b\n", 4); n = drain(m, b, sizeof(b), 1000, "DRN[");
			int ci = memfind(b,n,"cmd:");
			if (ci >= 0) { int e = ci; while (e < n && b[e] != '\r' && b[e] != '\n') e++;
				klog("   [T2 on-tty status line] %.*s\n", e-ci, b+ci); }
			int ok = count_sub(b,n,"cmd:")==1 && memfind(b,n,"SIGHIT")>=0
			         && !has_byte(b,n,ST) && memfind(b,n,"DRN[ab]")>=0;
			record("T2 cooked fire+splice+status", ok, "see log"); }
		else record("T2 cooked", 0, "no RDY");
		waitpid(pid,&st,0); close(m);
	} else record("T2 cooked", 0, "start failed");

	/* T3 raw mode: ^T passed through literally, no fire */
	C = (struct cfg){ N_TTY_INFO,0,0,0,0,1, 1,28,3,0 };
	if ((pid = start(&m)) > 0) {
		if (wait_rdy(m)) { write(m, "a\x14""b", 3); n = drain(m, b, sizeof(b), 1000, "]");
			record("T3 raw passthrough",
			       count_sub(b,n,"cmd:")==0 && memfind(b,n,"SIGHIT")<0 && has_byte(b,n,ST),
			       "^T should pass through in raw mode"); }
		else record("T3 raw", 0, "no RDY");
		waitpid(pid,&st,0); close(m);
	} else record("T3 raw", 0, "start failed");

	/* T4 ISIG on but ICANON off: still passthrough (proves ICANON gate) */
	C = (struct cfg){ N_TTY_INFO,0,1,1,0,0, 1,28,3,0 };
	if ((pid = start(&m)) > 0) {
		if (wait_rdy(m)) { write(m, "a\x14""b", 3); n = drain(m, b, sizeof(b), 1000, "]");
			record("T4 ISIG on/ICANON off passthrough",
			       count_sub(b,n,"cmd:")==0 && memfind(b,n,"SIGHIT")<0 && has_byte(b,n,ST),
			       "ICANON-off should not fire"); }
		else record("T4", 0, "no RDY");
		waitpid(pid,&st,0); close(m);
	} else record("T4", 0, "start failed");

	/* T5 multi-chunk cooked input: ^T mid-stream fires exactly once, all
	 * data bytes preserved in order, ^T spliced out. 4000 bytes stays under
	 * the 4096 canonical line limit so delivery is deterministic, while
	 * still spanning many flip-buffer chunks (several receive_buf2 calls),
	 * which is what exercises the segment-splitting + single-fire logic. */
	C = (struct cfg){ N_TTY_INFO,1,1,1,0,0, 1,28,4000,0 };
	if ((pid = start(&m)) > 0) {
		if (wait_rdy(m)) {
			static char big[4100]; int i;
			for (i=0;i<2000;i++) big[i]='x'; big[2000]=ST;
			for (i=0;i<2000;i++) big[2001+i]='x'; big[4001]='\n';
			write(m, big, 4002);
			n = drain(m, b, sizeof(b), 3000, "DRN:");
			int cc = count_sub(b,n,"cmd:"), drn = memfind(b,n,"DRN:");
			klog("   [T5 dbg] n=%d cmd:count=%d drn=\"%.20s\" raw0x14=%d\n", n, cc,
			     drn>=0 ? b+drn : "(none)", has_byte(b,n,ST));
			int ok = cc==1 && memfind(b,n,"DRN:4000:allx")>=0;
			record("T5 multi-chunk: single fire, bytes intact", ok, "see dbg");
		} else record("T5", 0, "no RDY");
		waitpid(pid,&st,0); close(m);
	} else record("T5", 0, "start failed");

	/* T6 non-handler child SURVIVES SIGWINCH(28) */
	C = (struct cfg){ N_TTY_INFO,1,1,1,0,0, 0,0,0,0 };
	if ((pid = start(&m)) > 0) {
		if (wait_rdy(m)) { write(m, "\x14", 1); n = drain(m, b, sizeof(b), 700, NULL);
			alive = (waitpid(pid, &st, WNOHANG) == 0);
			record("T6 non-handler survives SIGWINCH", alive && count_sub(b,n,"cmd:")==1,
			       alive ? "no fire" : "child died (28 should be ignored)"); }
		else record("T6", 0, "no RDY");
		kill(pid,SIGKILL); waitpid(pid,&st,0); close(m);
	} else record("T6", 0, "start failed");

	/* ---------------- cycle B: infosig=10 (SIGUSR1, fatal) ---------------- */
	if (delete_module("n_tty_info", O_NONBLOCK) < 0) { klog("  rmmod between cycles: %s\n", strerror(errno)); }
	if (load_mod("infosig=10 ttystatus=1") < 0) { klog("RESULT: FAIL (reload)\n"); goto done; }
	klog("reloaded infosig=10\n");

	/* T7 non-handler child DIES from SIGUSR1(10) */
	C = (struct cfg){ N_TTY_INFO,1,1,1,0,0, 0,0,0,0 };
	if ((pid = start(&m)) > 0) {
		if (wait_rdy(m)) { write(m, "\x14", 1); n = drain(m, b, sizeof(b), 1000, NULL);
			waitpid(pid, &st, 0);
			record("T7 non-handler dies from SIGUSR1",
			       WIFSIGNALED(st) && WTERMSIG(st)==10, "child not killed by 10"); }
		else { record("T7", 0, "no RDY"); kill(pid,SIGKILL); waitpid(pid,&st,0); }
		close(m);
	} else record("T7", 0, "start failed");

	/* T8 unload: EBUSY while a tty holds the ldisc, success after restore */
	if (open_pty(&m, &s) == 0) {
		int ld = N_TTY_INFO, n0 = N_TTY, sr, e2;
		int si = ioctl(s, TIOCSETD, &ld);
		r1 = delete_module("n_tty_info", O_NONBLOCK); e1 = errno;
		sr = ioctl(s, TIOCSETD, &n0); close(s); close(m);
		usleep(200000);	/* let any pty hangup settle */
		r2 = delete_module("n_tty_info", O_NONBLOCK); e2 = errno;
		klog("   [T8 dbg] setd=%d r1=%d e1=%s | restore=%d r2=%d e2=%s\n",
		     si, r1, strerror(e1), sr, r2, strerror(e2));
		/* in-use returns EWOULDBLOCK (EAGAIN) from __try_stop_module,
		 * or EBUSY on some versions -- accept either. */
		record("T8 rmmod busy-while-attached then OK",
		       r1 < 0 && (e1 == EWOULDBLOCK || e1 == EBUSY) && r2 == 0,
		       "unexpected rmmod behaviour");
	} else record("T8 unload", 0, "pty open failed");

	/* ---------------- cycle C: stackdump=1 ---------------- */
	if (load_mod("infosig=28 ttystatus=1 stackdump=1") < 0) { klog("RESULT: FAIL (load C)\n"); goto done; }
	klog("loaded stackdump=1\n");

	/* T9 stackdump: ^T dumps the fg task's kernel call trace to the log,
	 * and the status line shows a real wait channel (child sleeps in a pipe) */
	C = (struct cfg){ N_TTY_INFO,1,1,1,0,0, 0,0,0,0,1 };	/* block_pipe=1 */
	if ((pid = start(&m)) > 0) {
		if (wait_rdy(m)) {
			int km = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
			static char kb[16384]; int kt = 0;
			if (km >= 0) lseek(km, 0, SEEK_END);	/* only read records after here */
			usleep(300000);				/* let the child reach pipe_read */
			write(m, "\x14", 1);			/* fire while child sleeps in pipe_read */
			n = drain(m, b, sizeof(b), 800, NULL);
			usleep(300000);
			if (km >= 0) { for (;;) { int r = read(km, kb + kt, sizeof(kb) - 1 - kt);
				if (r <= 0) break; kt += r; if (kt >= (int)sizeof(kb) - 1) break; } kb[kt] = 0; }
			int ci = memfind(b, n, "cmd:");
			if (ci >= 0) { int e = ci; while (e < n && b[e] != '\r' && b[e] != '\n') e++;
				klog("   [T9 wchan line] %.*s\n", e - ci, b + ci); }
			int trace = memfind(kb, kt, "Call Trace") >= 0;
			klog("   [T9 dbg] kmsg bytes=%d Call-Trace=%d\n", kt, trace);
			record("T9 stackdump emits kernel call trace", trace, "no Call Trace in kmsg");
			if (km >= 0) close(km);
		} else record("T9", 0, "no RDY");
		kill(pid, SIGKILL); waitpid(pid, &st, 0); close(m);
	} else record("T9", 0, "start failed");

	/* T10 VLNEXT: ^V^T inserts a literal ^T (no fire); a later unquoted ^T
	 * still fires and is spliced -- proves the lnext state also clears. */
	C = (struct cfg){ N_TTY_INFO,1,1,1,0,0, 0,0,4,0,0 };	/* cooked, echo off, read 4 */
	if ((pid = start(&m)) > 0) {
		if (wait_rdy(m)) {
			write(m, "a\x16\x14""b\x14""c\n", 7);	/* a ^V ^T b ^T c \n */
			n = drain(m, b, sizeof(b), 1000, "DRN[");
			int cc = count_sub(b,n,"cmd:");
			klog("   [T10 dbg] cmd=%d has0x14=%d bc=%d\n", cc, has_byte(b,n,ST), memfind(b,n,"bc]")>=0);
			record("T10 ^V^T quotes literal ^T; next ^T still fires",
			       cc==1 && has_byte(b,n,ST) && memfind(b,n,"bc]")>=0, "see dbg");
		} else record("T10", 0, "no RDY");
		kill(pid,SIGKILL); waitpid(pid,&st,0); close(m);
	} else record("T10", 0, "start failed");

	/* T11 VLNEXT across receive_buf2 calls: ^V in one write, ^T in the next
	 * -- the quote must still apply (no fire), proving per-tty lnext persists. */
	C = (struct cfg){ N_TTY_INFO,1,1,1,0,0, 0,0,1,0,0 };	/* read the 1 literal byte */
	if ((pid = start(&m)) > 0) {
		if (wait_rdy(m)) {
			write(m, "\x16", 1); usleep(60000);	/* ^V, flushed on its own */
			write(m, "\x14", 1); usleep(60000);	/* ^T in a later buffer */
			write(m, "\n", 1);			/* flush the canonical line */
			n = drain(m, b, sizeof(b), 1000, "DRN[");
			int cc = count_sub(b,n,"cmd:");
			klog("   [T11 dbg] cmd=%d has0x14=%d\n", cc, has_byte(b,n,ST));
			record("T11 cross-call ^V then ^T quotes (no fire)",
			       cc==0 && has_byte(b,n,ST), "see dbg");
		} else record("T11", 0, "no RDY");
		kill(pid,SIGKILL); waitpid(pid,&st,0); close(m);
	} else record("T11", 0, "start failed");

	klog("---- %d passed, %d failed ----\n", passes, fails);
	klog("RESULT: %s (%d/%d)\n", fails == 0 ? "PASS" : "FAIL", passes, passes + fails);
done:
	sync();
	reboot(RB_POWER_OFF);
	reboot(RB_AUTOBOOT);
	for (;;) pause();
}

/* handshake helper (forward-declared above) */
static int wait_rdy(int m) { char b[128]; int n = drain(m, b, sizeof(b), 2000, "RDY"); return memfind(b, n, "RDY") >= 0; }
