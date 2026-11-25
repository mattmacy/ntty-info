# n_tty_info — FreeBSD-style `^T` / SIGINFO for Linux

A kernel module that adds FreeBSD's `VSTATUS` behaviour to Linux: pressing
**`^T`** at a terminal prints a status line and sends a signal to the
**foreground process group**, so a cooperating program can report progress. It
is a line discipline *derived* from `N_TTY` — it keeps all of N_TTY's normal
cooked-mode processing and only adds the `^T` handling, at the same layer
FreeBSD does it.

**Status: built and tested in a VM against kernel 7.0.0-43-generic — 11/11 checks
pass** (see [Tested](#tested)).

## What it is and is not

- **It is** the trigger at the correct layer: it fires even while a foreground
  program is busy or blocked in `read()`, which a shell keybinding cannot.
- **It is not** a real `SIGINFO`. Linux has no such signal, and a module cannot
  mint a new signal whose default action is *Ignore*. So you pick an existing
  signal (see [Choosing the signal](#choosing-the-signal-infosig)). This is the
  one semantic gap versus FreeBSD.

## Build

Requires the kernel headers for your running kernel
(`apt install linux-headers-$(uname -r)`) and a toolchain.

```sh
make all          # builds n_tty_info.ko, setinfold, infotest
```

## Load

```sh
sudo insmod ./n_tty_info.ko                 # default: ^T -> SIGUSR1, on-tty status line
# choose the signal / toggle the status outputs:
sudo insmod ./n_tty_info.ko infosig=28 ttystatus=1 logstatus=1
dmesg | tail -n2                            # confirm "registered as ldisc 29"
```

Parameters (all live-tunable via `/sys/module/n_tty_info/parameters/`):

| Param | Default | Meaning |
|-------|---------|---------|
| `infosig` | `SIGUSR1` (10) | signal sent to the foreground pgrp on `^T` |
| `ttystatus` | `1` | print a FreeBSD-style status line to the terminal on `^T` |
| `stackdump` | `0` | dump each foreground task's full kernel call trace to the log on `^T` |
| `logstatus` | `0` | also log (tty, signal, pgrp) to the kernel log on `^T` |

## Use

The discipline is **per-tty**, not per-process. Attach it to your terminal,
then every program you run there inherits it:

```sh
./setinfold          # attach to the current terminal
./infotest           # a test program; press ^T and watch it report
./setinfold -r       # restore the standard N_TTY discipline
```

The on-tty status line looks like (FreeBSD prints a similar line):

```
cmd: dd 1234 [D] 12.34u 0.56s wchan: blkdev_issue_flush
```

— command, pid, task state, user/system CPU time, and the **wait channel** of
the foreground group leader: the kernel function it is currently blocked in (the
top non-scheduler stack frame). That last field is the useful one — a bare `[R]`
or `[D]` doesn't tell you *where* a process is stuck. (FreeBSD also leads with
the load average; `get_avenrun()` is not exported to modules on modern Linux, so
that field is omitted.)

### Dumping the full kernel stack (`stackdump=1`)

The wait channel is one frame; for the whole picture, load with `stackdump=1`
and `^T` dumps every foreground task's complete kernel call trace to the kernel
log via `sched_show_task()`:

```
$ dmesg
task:dd    state:D stack:0 pid:1234 ...
Call Trace:
 blkdev_issue_flush+...
 ext4_sync_file+...
 ksys_fsync+...
 ...
```

This is the Linux analog of attaching a debugger to see exactly where a stuck
foreground job is sitting — much more than `[D]` tells you. It is verbose and
goes to the kernel log, so it defaults off.

To apply it automatically to interactive shells, add to `~/.bashrc`:

```sh
[ -t 0 ] && /path/to/setinfold 2>/dev/null
```

This is **safe** because of the gating below: `^T` at the shell's own prompt is
passed through to the line editor, not turned into a (possibly fatal) signal to
the shell.

## Choosing the signal (`infosig`)

This is the important decision. On Linux, almost no spare signal is *Ignore* by
default, so broadcasting to a whole process group can **kill** a program that
doesn't handle it:

| `infosig` | Signal   | Default action | Trade-off |
|-----------|----------|----------------|-----------|
| `10`      | SIGUSR1  | **Terminate**  | default here; GNU `dd` already reports progress on it, but kills non-handlers |
| `28`      | SIGWINCH | **Ignore** ✓   | safe to broadcast, but foreground apps will think the window resized and redraw |
| `30`      | SIGPWR   | **Terminate**  | rarely used, but kills non-handlers |

Recommendation: if you only `^T` programs you control (or that handle the
signal), keep `SIGUSR1`. If you want "harmless when nobody's listening" like
FreeBSD, use `infosig=28` (SIGWINCH) and accept the occasional redraw.

`dd` tip: with the default `infosig=10`, `^T` makes GNU `dd` print its transfer
status — no recompile needed.

## How it works

- `n_tty_inherit_ops()` copies N_TTY's `tty_ldisc_ops`; the module overrides
  only `receive_buf2`.
- On input it processes the buffer in segments split at `^T`. Each non-`^T`
  segment is handed to the original N_TTY `receive_buf2` (so echo, erase, line
  editing all behave normally). At each `^T` it prints the status line via
  `tty->ops->write()`, optionally dumps each foreground task's call trace
  (`sched_show_task()`), signals the foreground pgrp with `kill_pgrp()`, and
  **swallows the byte** so it is neither echoed nor delivered.
- The status line's wait channel comes from `stack_trace_save_tsk()` +
  `sprint_symbol()` on the group leader (skipping scheduler frames); it is only
  computed for an off-CPU task, else it reads `running`.
- **Gating:** it only acts when `ISIG && ICANON && IEXTEN` are all set — the
  same combination FreeBSD requires for `VSTATUS`. This is also what makes it
  safe: line editors (readline, zsh ZLE) and full-screen apps (vi, less, tmux)
  run with `ICANON` cleared, so `^T` passes straight through to them and never
  signals the interactive shell. It still fires for an ordinary cooked-mode
  foreground job (`dd`, `cp`, `make`, `ping`), which doesn't change tty modes.
- A parity/frame/break-flagged `0x14` is treated as ordinary data, not `^T`.
- `VLNEXT` (`^V`) quoting is honoured: a `^V`-quoted `^T` is left in the stream
  for N_TTY to insert literally, not fired/swallowed. The module mirrors N_TTY's
  `lnext` state in a small per-tty side table (it can't touch N_TTY's private
  `disc_data`), populated by wrapping the inherited `open`/`close`. The state
  persists across `receive_buf2` calls (a `^V` can end one buffer and the quoted
  byte begin the next) and is committed only for bytes N_TTY actually consumed,
  so a partial-consumption retry doesn't double-count `^V` transitions.
- Partial-consumption (N_TTY's 4 KiB read buffer filling mid-buffer) is handled
  without double-firing: a `^T` is only passed once its preceding segment is
  fully consumed, and the unconsumed tail the core retries always begins after
  it.

## Unloading safely

The module holds a reference while any tty uses the discipline, so `rmmod` fails
(`EWOULDBLOCK`/`EBUSY`) until you release it:

```sh
./setinfold -r          # on every terminal still using it
sudo rmmod n_tty_info
```

## Tested

Built against `linux-headers-7.0.0-43-generic` (vermagic exact match) and run
inside a QEMU/KVM guest booting that same kernel, via a static-init initramfs
that loads the module and drives pty pairs. All checks pass, no kernel splats,
clean power-off:

| Check | Result |
|-------|--------|
| T1 control — under plain N_TTY, `^T` is literal, no signal | PASS |
| T2 cooked — fire + byte-splice + status line + signal delivered | PASS |
| T3 raw mode — `^T` passes through, no fire | PASS |
| T4 `ISIG` on / `ICANON` off — passes through (proves the ICANON gate) | PASS |
| T5 multi-chunk cooked input — exactly one fire, all bytes intact, `^T` spliced | PASS |
| T6 non-handler child survives `SIGWINCH` (ignore-default) | PASS |
| T7 non-handler child dies from `SIGUSR1` (terminate-default) | PASS |
| T8 `rmmod` refused while attached, succeeds after restore | PASS |
| T9 `stackdump=1` emits a kernel call trace; status line shows a real wait channel (`wchan: anon_pipe_read`) | PASS |
| T10 `^V^T` inserts a literal `^T` (no fire); a following unquoted `^T` still fires and is spliced | PASS |
| T11 VLNEXT across `receive_buf2` calls — `^V` in one buffer, `^T` in the next — still quotes (per-tty state persists) | PASS |

Reproduce: `cd vm && ./run.sh` (see `vm/` for the harness).

## Kernel version notes

Written for the modern line-discipline API (kernel ≈ 6.6+), where
`receive_buf2` is:

```c
size_t (*receive_buf2)(struct tty_struct *, const u8 *cp, const u8 *fp, size_t count);
```

- **5.14 – ~6.5:** the prototype was
  `int (*receive_buf2)(struct tty_struct *, const unsigned char *cp, const char *fp, int count);`
  — change the signatures of `info_receive_buf2` and `orig_receive_buf2`
  accordingly (the body logic is identical).
- **< 5.14:** `tty_register_ldisc()`/`tty_unregister_ldisc()` take an `int disc`
  argument — the module already `#if`-guards this on `LINUX_VERSION_CODE`.

## Security / caveats

- Loading a line discipline requires root and is powerful; only load code you
  trust. Keep the signal choice in mind — a careless `infosig` can terminate
  foreground jobs that don't handle it. A cooked-mode shell builtin (`read`)
  makes the shell the foreground pgrp, so `^T` there signals the shell; with a
  fatal `infosig` that ends the builtin. Prefer `infosig=28` if that matters.
- The slot is `N_DEVELOPMENT` (29), the number the kernel reserves for
  "manual out-of-tree testing" (`include/uapi/linux/tty.h`).
- The on-tty status line is written with `tty->ops->write()` (the same call
  N_TTY uses for echo from this path). It bypasses `OPOST` — so CR/LF are
  emitted explicitly — and is not serialised against the foreground program's
  own output, so a status line can interleave with concurrent output. Cosmetic,
  never a crash.
- Known limitation: swallowing a `^T` that N_TTY's `lookahead_buf` already
  counted can leave its `lookahead_count` one too high, so a subsequent
  `^S`/`^Q` may be skipped right after a buffer-full event. Harmless in
  practice; left unaddressed.
- Note: `^T` injected via the `TIOCSTI` ioctl is not intercepted. The module
  overrides only `receive_buf2`; `TIOCSTI` reaches N_TTY's inherited
  `receive_buf`, so such a `^T` is delivered literally rather than swallowed.
  (`TIOCSTI` requires `CAP_SYS_ADMIN` by default.)
