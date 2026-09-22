# ECEN 602 Machine Problem 1: TCP Echo Server and Client

RFC 862 echo service over TCP. The server forks a child per connection, so
multiple clients are served simultaneously.

## Team and roles

| Member | Role |
|---|---|
| Bozhou Chen (Kotori) | Server (`echos.c`), shared I/O (`echo_io.c`), `util.c`, Makefile, test harness |
| Shih-Yu (Frank) Yang | Client (`echo.c`), shared framing interface (`echo_io.h`), test report screen captures |

## Build and usage

```sh
make            # builds echos and echo
make test       # runs the automated harness (with 11 cases implemented in test_echos.c)
make clean      # removes binaries and object files

./echos <port>              # e.g. ./echos 9001
./echo <ip_addr> <port>     # e.g. ./echo 127.0.0.1 9001
```

`gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -g`, no warnings on gcc 13.3
(Ubuntu 24.04). Only libc and the POSIX socket API.

`<port>` must be 1–65535; below 1024 requires root. Ctrl-C stops the server
cleanly rather than killing it.

## Files

| File | Contents |
|---|---|
| `echos.c` | Server |
| `echo_io.c`, `echo_io.h` | `writen()` and `readline()`, shared by both Client and Server |
| `util.c`, `util.h` | Timestamped logging and fatal-error reporting |
| `echo.c` | Client |
| `docs/DESIGN.md` | Conceptual model and design decisions |
| `docs/Test_Report/` | Screen captures for the test report |
| `tests/test_echos.c` | Automated harness, 11 cases, non-zero exit on failure |
| `tests/three_clients.sh` | Hand-driven three-client demo |

## Architecture

```
parent                                   child (one per connection)
------                                   --------------------------
socket() → setsockopt(SO_REUSEADDR)
        → bind() → listen(128)
  │
  ├─ accept() ──── fork() ──────────────▶ close(listenfd)
  │                  │                    loop:
  │                  │                      readline(connfd, buf, MAXLINE+1)
  ├─ close(connfd)   │                      → 0  : EOF → exit
  │                  │                      → n  : writen(connfd, buf, n)
  └─ loop to accept()                       → -1 : log error → exit(1)
                                           close(connfd); _exit()
```

Each child has its own address space, so per-connection state, including
`readline()`'s buffer, cannot leak between clients.

Key decisions:

- `fork()` per connection, as the handout requires; it also isolates a
  misbehaving client to its own child.
- Parent closes `connfd`, child closes `listenfd`. Both are mandatory: an
  open `connfd` in the parent keeps the client from ever seeing FIN, and an
  open `listenfd` in a child keeps the port bound after the parent dies.
- `SIGCHLD` reaped with `sigaction()` and a `waitpid(WNOHANG)` loop. Signals
  do not queue, so one `SIGCHLD` can cover several exits.
- `SIGPIPE` ignored, so a vanished client surfaces as `EPIPE` to log
  instead of killing the child silently.
- `EINTR` handled in `readline()`, `writen()` and around `accept()`.
- `SO_REUSEADDR`, or a restart inside `TIME_WAIT` fails with `EADDRINUSE`.
- Graceful shutdown: `SIGINT`/`SIGTERM` without `SA_RESTART` so `accept()`
  returns `EINTR`; `SIGCHLD` with `SA_RESTART` so it does not.

## Long lines

`ECHO_MAXLINE` is 4096 and buffers are `ECHO_MAXLINE + 1`, per the handout.
This is a buffer size, not a protocol limit: a longer line is echoed back in
4096-byte pieces with nothing lost and the connection left open. Verified —
5000 bytes sent with no newline come back as 5000 bytes.

## Miscellaneous

1. `readline()` return value. Another version returns `maxlen` when a line
   fills the buffer although only `maxlen - 1` bytes were stored. This one returns
   the count actually stored, so it can be passed straight to`writen()`.
2. Not thread-safe. `readline()` keeps static state, as the handout allows.
3. Log ordering. Each line is emitted with one `write()` so children never
   interleave mid-line, but ordering between processes is up to the scheduler.
4. IPv4 only, per the handout's dotted-decimal requirement.
5. No idle timeout. A client that connects and sends nothing keeps its
   child alive until it disconnects.
6. From Bozhou: In the Test Report, I mistakenly put my umich email
   instead of my tamu email, but my NetID is indeed Kotori, and my tamu
   email is kotori@tamu.edu.