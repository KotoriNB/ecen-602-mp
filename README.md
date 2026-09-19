# ECEN 602 — Machine Problem 1: TCP Echo Server and Client

RFC 862 echo service over TCP. The server (`echos`) is concurrent: it forks a
child process per connection, so multiple clients are served simultaneously.

## Team and roles

| Member | Role |
|---|---|
| Bozhou Chen (Kotori) | Server (`echos.c`), shared I/O (`echo_io.c`), `util.c`, Makefile, test harness |
| *(partner name)* | Client (`echo.c`), test report screen captures |

> Fill in the partner's name before submitting — Submission Guideline 1 asks
> for a statement of each member's role.

## Build

```sh
make            # builds echos (and echo, once echo.c is present)
make clean      # removes all binaries and object files — run before submitting
```

Built with `gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -g`. No warnings on
gcc 13.3 (Ubuntu 24.04). No third-party libraries; only libc and the POSIX
socket API.

## Usage

```sh
./echos <port>              # server, e.g. ./echos 9001
./echo <ip_addr> <port>     # client, e.g. ./echo 127.0.0.1 9001
```

`<port>` must be an integer in 1–65535; anything else is rejected with a
message rather than silently coerced. Ports below 1024 require root.

Stop the server with Ctrl-C — it closes the listening socket and exits
cleanly rather than being killed.

## Files

| File | Contents |
|---|---|
| `echos.c` | Concurrent echo server: socket/bind/listen/accept/fork loop, signal handling, per-connection echo service |
| `echo_io.c`, `echo_io.h` | `writen()` and `readline()` — the two functions the handout asks us to write. Shared by client and server |
| `util.c`, `util.h` | Timestamped logging and `err_sys`-style fatal-error reporting |
| `echo.c` | Client (partner's half) |
| `Makefile` | `all`, `clean`, `test`, `dist` |
| `docs/DESIGN.md` | Conceptual model / architecture sketch (required by Note 2) |
| `docs/TESTPLAN.md` | Test strategy, the five required cases, how to reproduce each |
| `docs/screenshots/` | Screen captures for the test report |
| `tests/test_echos.c` | Automated harness in C — 11 cases, exits non-zero on failure |
| `tests/three_clients.sh` | Hand-driven three-client demo for the screenshots |

## Architecture

```
parent                                   child (one per connection)
------                                   --------------------------
socket()
setsockopt(SO_REUSEADDR)
bind()
listen(backlog=128)
  │
  ├─ accept() ──── fork() ──────────────▶ close(listenfd)
  │                  │                    loop:
  │                  │                      readline(connfd, buf, MAXLINE+1)
  ├─ close(connfd)   │                      → 0  : EOF, client closed → exit
  │                  │                      → n  : writen(connfd, buf, n)
  └─ loop back to accept()                   → -1 : log error → exit(1)
                                           close(connfd); _exit()
```

The parent never touches connection data. Each child has its own address
space, so per-connection state (including `readline()`'s buffer) cannot leak
between clients.

### Key decisions

**`fork()` per connection, not threads or `select()`.** The handout asks for
`fork()`. It also buys isolation for free: a wedged or hostile client can only
affect its own child.

**The parent closes `connfd`, the child closes `listenfd`.** Both are
mandatory, not hygiene. If the parent kept `connfd` open, the reference count
would never reach zero and the client would never see FIN when the child
exits. If the child kept `listenfd`, the port would stay bound after the
parent died.

**Zombie reaping.** `SIGCHLD` is handled with `sigaction()` and the handler
loops on `waitpid(-1, NULL, WNOHANG)`. The loop is required: standard signals
do not queue, so several children exiting close together can produce a single
`SIGCHLD`. The handler saves and restores `errno` so it cannot corrupt an
interrupted main-line system call. Verified with `ps` in test case 6.

**`SIGPIPE` ignored.** If the client disappears mid-echo, the default action
would kill the child silently. Ignoring it makes `writen()` fail with `EPIPE`,
which we log.

**`EINTR` handled at every slow system call** — inside `readline()`,
inside `writen()`, and around `accept()` — so a signal never turns into a
spurious error or a short count for the caller.

**`SO_REUSEADDR`.** Without it, restarting the server inside the 2×MSL
`TIME_WAIT` window fails with `EADDRINUSE`, which makes iterative testing
miserable.

**Buffered `readline()`.** The efficient version from the handout: `read()`
fills a private 4096-byte buffer and characters are handed out one at a time,
so a full line costs about one system call instead of one per character. The
static state makes it non-reentrant and not thread-safe — acceptable here
because concurrency comes from separate *processes*, and `readline_reset()` is
called before each connection anyway.

**Graceful shutdown.** `SIGINT`/`SIGTERM` are installed *without* `SA_RESTART`
so a blocked `accept()` returns `EINTR`; the loop notices the flag, closes the
listening socket and exits. `SIGCHLD` *is* installed with `SA_RESTART` so a
child exiting does not disturb `accept()`.

### Long lines

`ECHO_MAXLINE` is 4096, and buffers are `ECHO_MAXLINE + 1` bytes so there is
always room for the `'\0'`, exactly as the handout specifies.

This is a buffer size, **not** a protocol limit. A line longer than 4096 bytes
is returned by `readline()` in 4096-byte pieces (without a trailing newline)
and each piece is echoed immediately. No bytes are lost, nothing is truncated,
and the connection is not torn down. Verified: 5000 bytes sent with no newline
come back as 5000 bytes.

### Errata / known deviations

1. **`readline()` return value.** Stevens' UNP version returns `maxlen` when a
   line fills the buffer, although only `maxlen - 1` bytes were stored — an
   off-by-one. This implementation returns the number of bytes actually
   stored, excluding the `'\0'`. Callers can pass the return value straight to
   `writen()`, which is what the server does.
2. **Not thread-safe.** `readline()` keeps static state, as the handout allows.
3. **Log interleaving.** Each log line is formatted into a stack buffer and
   emitted with a single `write()`, so lines from several children do not
   interleave mid-line. Ordering between processes is still whatever the
   scheduler decides.
4. **IPv4 only.** `AF_INET`, per the handout ("IPv4 address in dotted decimal
   notation"). Moving to `getaddrinfo()`/`AF_INET6` would be the next step.
5. **No idle timeout.** A client that connects and never sends anything keeps
   its child alive until it disconnects. Fine for this assignment; a
   production server would want a timer.

## Testing

```sh
make test       # 11 automated cases, all five required ones included
```

See `docs/TESTPLAN.md` for the strategy, the mapping to the five required
cases, and instructions for reproducing each by hand for the screen captures.

## References

- W. R. Stevens, B. Fenner, A. M. Rudoff, *Unix Network Programming, Vol. 1*,
  3rd ed., Ch. 1, 3, 4, 5, 26.
- J. Postel, *RFC 862: Echo Protocol*, May 1983.
