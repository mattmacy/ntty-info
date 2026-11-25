// SPDX-License-Identifier: GPL-2.0
/*
 * n_tty_info - a line discipline derived from N_TTY that turns a status
 * character (default ^T, 0x14) into a signal delivered to the foreground
 * process group of the tty, emulating FreeBSD's VSTATUS / SIGINFO behaviour.
 *
 * It inherits all of N_TTY's cooked-mode processing via n_tty_inherit_ops()
 * and overrides only the input path (receive_buf2): it scans incoming bytes
 * for ^T, swallows that byte (so it is not echoed or delivered to the app),
 * and signals the foreground pgrp -- exactly where FreeBSD does it, in the
 * line discipline.
 *
 * Targets a modern kernel (>= 6.6, where receive_buf2 uses `const u8 *` and
 * `size_t`). See README.md for the pre-6.6 / pre-5.14 prototype adjustments.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/tty.h>
#include <linux/tty_ldisc.h>
#include <linux/tty_buffer.h>	/* TTY_NORMAL */
#include <linux/sched/signal.h>
#include <linux/sched.h>
#include <linux/math64.h>
#include <linux/rcupdate.h>
#include <linux/string.h>
#include <linux/kallsyms.h>	/* sprint_symbol, KSYM_SYMBOL_LEN */
#include <linux/stacktrace.h>	/* stack_trace_save_tsk */
#include <linux/sched/debug.h>	/* sched_show_task */
#include <linux/slab.h>		/* kzalloc/kfree */
#include <linux/spinlock.h>
#include <linux/hashtable.h>
#include <linux/pid.h>
#include <linux/version.h>

#ifndef N_TTY_INFO
/*
 * N_DEVELOPMENT (29) is the slot the kernel reserves for exactly this purpose:
 * "Manual out-of-tree testing" (include/uapi/linux/tty.h). Fall back to the
 * literal number on older headers that predate the name.
 */
#ifdef N_DEVELOPMENT
#define N_TTY_INFO N_DEVELOPMENT
#else
#define N_TTY_INFO 29
#endif
#endif

#define STATUS_CHAR 0x14	/* ^T */

static int infosig = SIGUSR1;
module_param(infosig, int, 0644);
MODULE_PARM_DESC(infosig,
	"signal number sent to the foreground pgrp on ^T (default SIGUSR1). "
	"Use 28 (SIGWINCH) for broadcast safety -- its default action is Ignore, "
	"so it will not kill a program that does not handle it.");

static bool logstatus;
module_param(logstatus, bool, 0644);
MODULE_PARM_DESC(logstatus,
	"also log (tty, signal, pgrp) to the kernel log on ^T");

static bool ttystatus = true;
module_param(ttystatus, bool, 0644);
MODULE_PARM_DESC(ttystatus,
	"print a FreeBSD-style status line to the terminal itself on ^T (default on)");

static bool stackdump;
module_param(stackdump, bool, 0644);
MODULE_PARM_DESC(stackdump,
	"on ^T, dump each foreground task's kernel call trace to the kernel log "
	"(sched_show_task); more informative than the status line's wait channel");

static struct tty_ldisc_ops info_ldisc;
static size_t (*orig_receive_buf2)(struct tty_struct *tty, const u8 *cp,
				   const u8 *fp, size_t count);
static int  (*orig_open)(struct tty_struct *tty);
static void (*orig_close)(struct tty_struct *tty);

/*
 * Per-tty state: just whether N_TTY's VLNEXT quoting (^V) is pending, so we can
 * mirror it and NOT treat a ^V-quoted ^T as the status char. N_TTY keeps this
 * in its private struct n_tty_data (tty->disc_data, which we must not touch),
 * so we track our own copy in a side table keyed by tty. A ^V may end one
 * receive_buf2 buffer and the quoted byte begin the next, so the state must
 * persist across calls -- hence it can't be a local.
 */
struct info_tty {
	struct tty_struct *tty;
	bool lnext;			/* previous processed byte was VLNEXT */
	struct hlist_node node;
};
static DEFINE_SPINLOCK(info_tbl_lock);
static DEFINE_HASHTABLE(info_tbl, 6);	/* 64 buckets; only a few ttys in practice */

static struct info_tty *info_find(struct tty_struct *tty)
{
	struct info_tty *it, *found = NULL;

	/*
	 * The ldisc core holds a reference across receive_buf2, so close()
	 * cannot free this tty's entry concurrently -- the returned pointer
	 * stays valid for the rest of the call once we drop the table lock.
	 */
	spin_lock(&info_tbl_lock);
	hash_for_each_possible(info_tbl, it, node, (unsigned long)tty) {
		if (it->tty == tty) { found = it; break; }
	}
	spin_unlock(&info_tbl_lock);
	return found;
}

static int info_open(struct tty_struct *tty)
{
	struct info_tty *it;
	int ret = orig_open(tty);

	if (ret)
		return ret;
	/* Allocation failure just disables VLNEXT tracking for this tty. */
	it = kzalloc(sizeof(*it), GFP_KERNEL);
	if (it) {
		it->tty = tty;
		spin_lock(&info_tbl_lock);
		hash_add(info_tbl, &it->node, (unsigned long)tty);
		spin_unlock(&info_tbl_lock);
	}
	return 0;
}

static void info_close(struct tty_struct *tty)
{
	struct info_tty *it, *found = NULL;

	spin_lock(&info_tbl_lock);
	hash_for_each_possible(info_tbl, it, node, (unsigned long)tty) {
		if (it->tty == tty) { found = it; hash_del(&it->node); break; }
	}
	spin_unlock(&info_tbl_lock);
	kfree(found);
	orig_close(tty);
}

/*
 * Advance the VLNEXT state over `len` bytes that are all being handed to N_TTY
 * (i.e. no swallowed status chars among them). Mirrors N_TTY: a VLNEXT char
 * arms lnext for exactly the next byte, which is then taken literally.
 */
static bool info_advance_lnext(const u8 *cp, const u8 *fp, size_t len,
			       bool lnext, u8 vlnext, bool vlnext_en)
{
	size_t k;

	for (k = 0; k < len; k++) {
		bool normal = (!fp || fp[k] == TTY_NORMAL);

		if (lnext)
			lnext = false;			/* this byte was quoted */
		else if (vlnext_en && normal && cp[k] == vlnext)
			lnext = true;			/* arm for the next byte */
	}
	return lnext;
}

/*
 * Derive a wait channel: the name of the kernel function the task is blocked
 * in -- the top stack frame that is not scheduler plumbing. Far more useful
 * than the bare run-state character. Only meaningful for an off-CPU task; for a
 * running one we just say "running". Caller holds rcu_read_lock.
 */
static void info_wchan(struct task_struct *task, char *out, size_t outsz)
{
	unsigned long entries[12];
	char sym[KSYM_SYMBOL_LEN];
	unsigned int n, i;

	if (task_is_running(task)) { strscpy(out, "running", outsz); return; }

	n = stack_trace_save_tsk(task, entries, ARRAY_SIZE(entries), 0);
	for (i = 0; i < n; i++) {
		char *plus;
		sprint_symbol(sym, entries[i]);
		if (!strncmp(sym, "__schedule", 10) ||
		    !strncmp(sym, "schedule", 8) ||
		    !strncmp(sym, "io_schedule", 11))
			continue;
		plus = strchr(sym, '+');	/* "sym+off/size [mod]" form */
		if (!plus)
			continue;	/* unresolved addr: don't leak it to the tty */
		*plus = '\0';			/* drop the +offset/size suffix */
		strscpy(out, sym, outsz);
		return;
	}
	strscpy(out, n ? "?" : "-", outsz);
}

/*
 * Build and emit a FreeBSD-style status line directly onto the terminal, e.g.
 *
 *   cmd: dd 1234 [S] 1.23u 0.45s wchan: pipe_read
 *
 * The wchan field is the kernel function the task is blocked in (the top
 * non-scheduler stack frame) -- far more useful than the bare run-state char.
 * For a deeper view, the stackdump param dumps the whole call trace to the log.
 *
 * (FreeBSD's line also leads with the load average, but get_avenrun() is not
 * exported to modules on modern Linux, so we omit it rather than resort to
 * kallsyms tricks. The command/state/CPU-time fields need no exported symbols.)
 *
 * This mirrors what FreeBSD's kernel prints from its VSTATUS handler. We write
 * via tty->ops->write(), which is exactly how N_TTY emits echo from this same
 * receive path, so the calling context is safe. Caveats (documented in
 * README.md): it bypasses OPOST (so we emit CR/LF ourselves) and is not
 * serialised against the foreground program's own output, so a line can
 * interleave with concurrent output -- cosmetic, never a crash.
 */
static void info_print_status(struct tty_struct *tty, struct pid *pgrp)
{
	char buf[200];
	struct task_struct *task;
	char comm[TASK_COMM_LEN] = "?";
	char wchan[48] = "-";
	pid_t pid = 0;
	char state = '?';
	u64 ut = 0, st = 0, ut_s, st_s;
	u32 rem, ut_cs = 0, st_cs = 0;
	int len;

	if (!tty->ops || !tty->ops->write)
		return;

	rcu_read_lock();
	task = pid_task(pgrp, PIDTYPE_PGID);
	if (task) {
		strscpy(comm, task->comm, sizeof(comm));
		pid = task_pid_nr(task);
		state = task_state_to_char(task);
		ut = task->utime;
		st = task->stime;
		info_wchan(task, wchan, sizeof(wchan));
	}
	rcu_read_unlock();

	/*
	 * comm is user-controlled (prctl PR_SET_NAME) and we emit it to the
	 * terminal via ops->write() with OPOST bypassed, so strip control and
	 * 8-bit bytes to prevent terminal escape-sequence injection.
	 */
	{
		char *p;

		for (p = comm; *p; p++) {
			unsigned char ch = (unsigned char)*p;

			if (ch < 0x20 || ch >= 0x7f)
				*p = '?';
		}
	}

	ut_s = div_u64_rem(ut, NSEC_PER_SEC, &rem);
	ut_cs = rem / (NSEC_PER_SEC / 100);
	st_s = div_u64_rem(st, NSEC_PER_SEC, &rem);
	st_cs = rem / (NSEC_PER_SEC / 100);

	len = scnprintf(buf, sizeof(buf),
		"\r\ncmd: %s %d [%c] %llu.%02uu %llu.%02us wchan: %s\r\n",
		comm, pid, state, ut_s, ut_cs, st_s, st_cs, wchan);

	tty->ops->write(tty, buf, len);
}

static void info_fire(struct tty_struct *tty)
{
	struct pid *pgrp = tty_get_pgrp(tty);

	if (!pgrp)
		return;

	if (logstatus)
		pr_info("n_tty_info: ^T on %s -> signal %d to pgrp %d\n",
			tty_name(tty), infosig, pid_vnr(pgrp));

	/* FreeBSD prints the status line, then signals the pgrp -- same order. */
	if (ttystatus)
		info_print_status(tty, pgrp);

	if (stackdump) {
		struct task_struct *p;
		/* sched_show_task() uses printk only (no sleep), so it is safe
		 * to call for each group member under rcu_read_lock. */
		rcu_read_lock();
		do_each_pid_task(pgrp, PIDTYPE_PGID, p) {
			sched_show_task(p);
		} while_each_pid_task(pgrp, PIDTYPE_PGID, p);
		rcu_read_unlock();
	}

	kill_pgrp(pgrp, infosig, 1);
	put_pid(pgrp);
}

static size_t info_receive_buf2(struct tty_struct *tty, const u8 *cp,
				const u8 *fp, size_t count)
{
	size_t processed = 0;
	size_t i = 0;

	/*
	 * Gate like FreeBSD's VSTATUS: ISIG + ICANON + IEXTEN. This is also the
	 * safe choice -- line editors (readline, zsh ZLE) and full-screen apps
	 * (vi, less, tmux) run with ICANON cleared, so ^T passes straight
	 * through to them and never signals (or, with a fatal infosig, kills)
	 * an interactive shell. It still fires for an ordinary cooked-mode
	 * foreground job (dd, cp, make, ping), which does not touch tty modes.
	 */
	struct info_tty *state = NULL;
	u8 vlnext;
	bool vlnext_en, lnext;

	if (!(L_ISIG(tty) && L_ICANON(tty) && L_IEXTEN(tty))) {
		/* Not in the gate: clear any pending quote so it can't leak
		 * across a mode switch, and pass everything through. */
		state = info_find(tty);
		if (state)
			state->lnext = false;
		return orig_receive_buf2(tty, cp, fp, count);
	}

	state = info_find(tty);
	vlnext = LNEXT_CHAR(tty);
	vlnext_en = (vlnext != __DISABLED_CHAR);	/* IEXTEN already checked */
	lnext = state ? state->lnext : false;

	while (i < count) {
		size_t j = i, seg;
		bool scan = lnext;

		/*
		 * Find the next status char, tracking VLNEXT as N_TTY would. A
		 * ^T only fires if it is unflagged real input AND is not quoted
		 * by a preceding ^V -- a ^V-quoted ^T (and a parity/frame/break
		 * marked one) is left in the stream for N_TTY to insert
		 * literally. `scan` ends holding the lnext state as of byte j.
		 */
		while (j < count) {
			bool normal = (!fp || fp[j] == TTY_NORMAL);

			if (scan)
				scan = false;		/* quoted: literal, skip */
			else if (vlnext_en && normal && cp[j] == vlnext)
				scan = true;		/* ^V: arm for next byte */
			else if (normal && cp[j] == STATUS_CHAR)
				break;			/* unquoted ^T: fire point */
			j++;
		}

		seg = j - i;
		if (seg) {
			size_t m = orig_receive_buf2(tty, cp + i,
						     fp ? fp + i : NULL, seg);

			processed += m;
			if (m < seg) {
				/*
				 * N_TTY's read buffer filled; it took only `m`
				 * of this segment and the core will re-push the
				 * rest later. Commit lnext for exactly the bytes
				 * consumed so the retry (which re-walks the tail)
				 * doesn't double-count VLNEXT transitions. We
				 * never re-fire on retry because a ^T is only
				 * passed once its preceding segment fully drains.
				 */
				lnext = info_advance_lnext(cp + i, fp ? fp + i : NULL,
							   m, lnext, vlnext, vlnext_en);
				goto out;
			}
		}
		/* Segment fully consumed: `scan` already is lnext as of byte j. */
		lnext = scan;

		if (j < count) {
			/* cp[j] == unquoted ^T: fire and swallow. N_TTY never
			 * sees it, so lnext is unchanged across the fire. */
			info_fire(tty);
			processed += 1;
			i = j + 1;
		} else {
			break;
		}
	}

out:
	if (state)
		state->lnext = lnext;
	return processed;
}

static int __init info_init(void)
{
	int ret;

	n_tty_inherit_ops(&info_ldisc);
	orig_receive_buf2	= info_ldisc.receive_buf2;
	orig_open		= info_ldisc.open;
	orig_close		= info_ldisc.close;
	info_ldisc.owner	= THIS_MODULE;
	info_ldisc.num		= N_TTY_INFO;
	info_ldisc.name		= "n_tty_info";
	info_ldisc.receive_buf2	= info_receive_buf2;
	info_ldisc.open		= info_open;	/* wrap to track per-tty VLNEXT */
	info_ldisc.close	= info_close;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
	ret = tty_register_ldisc(&info_ldisc);
#else
	ret = tty_register_ldisc(N_TTY_INFO, &info_ldisc);
#endif
	if (ret) {
		pr_err("n_tty_info: tty_register_ldisc failed: %d\n", ret);
		return ret;
	}

	pr_info("n_tty_info: registered as ldisc %d; ^T -> signal %d\n",
		N_TTY_INFO, infosig);
	return 0;
}

static void __exit info_exit(void)
{
	struct info_tty *it;
	struct hlist_node *tmp;
	unsigned int bkt;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
	tty_unregister_ldisc(&info_ldisc);
#else
	tty_unregister_ldisc(N_TTY_INFO);
#endif
	/* The module ref held per open tty blocks rmmod while any tty uses the
	 * discipline, so the table should be empty here -- sweep as a safety. */
	spin_lock(&info_tbl_lock);
	hash_for_each_safe(info_tbl, bkt, tmp, it, node) {
		hash_del(&it->node);
		kfree(it);
	}
	spin_unlock(&info_tbl_lock);
	pr_info("n_tty_info: unregistered\n");
}

module_init(info_init);
module_exit(info_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("n_tty_info");
MODULE_DESCRIPTION("N_TTY-derived line discipline: ^T signals the foreground pgrp (FreeBSD SIGINFO-style)");
