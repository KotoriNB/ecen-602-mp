# MP1 — Conceptual model and design sketch

## 1. What the service is

RFC 862: a server listens on a TCP port; once a connection is established, any
data received is sent back unchanged until the client terminates the connection.
The clientreads a line from stdin, sends it, the server echoes it, the client prints it.

## 2. Data model

| Scope | State | Lifetime |
|---|---|---|
| Server parent | `listenfd`, shutdown flag | process lifetime |
| Server child | `connfd`, peer address string, one `ECHO_MAXLINE + 1` byte line buffer, `readline()`'s private read buffer | one connection |
| Client | `sockfd`, one line buffer | one connection |

## 3. Major decision points

### D1. Concurrency model — `fork()` per connection

Chosen: `fork()`.

### D2. Where does the framing happen?

Both sides use `readline()`. The server never assumes a `write()` from the
client maps to one `read()` on its side: TCP is a byte stream, and a line can
arrive in any number of segments. `readline()` is what turns the stream back
into lines. Test case 7 sends one line in five segments to prove it.

### D3. Line length

`ECHO_MAXLINE = 4096`, buffers are `ECHO_MAXLINE + 1`.

Decision: an over-long line is not an error and not truncated. It is echoed 
in `ECHO_MAXLINE`-sized pieces. The alternative — dropping the excess, or 
closing the connection — would lose user data and violate Postel's "be
liberal in what you accept". Client and server must agree on the constant only
for the *max-length-line* test case to be meaningful, not for correctness.

### D4. Termination

| Event | Detection | Action |
|---|---|---|
| Client sends EOF (Ctrl-D) | `read()` returns 0 → `readline()` returns 0 | Child logs, closes `connfd`, `_exit(0)` |
| Client process killed | `read()` returns `ECONNRESET`, or `write()` gives `EPIPE` | Child logs the error, `_exit(1)` |
| Operator stops the server | `SIGINT`/`SIGTERM` → flag → `accept()` returns `EINTR` | Parent closes `listenfd`, reaps, exits 0 |
| Child finished | `SIGCHLD` | Handler loops on `waitpid(WNOHANG)` |

### D5. Error policy

Every system call return is checked. Failures split three ways:

- **Fatal for the whole server** (`socket`, `bind`, `listen`, `sigaction`):
  log with `strerror(errno)` and exit. There is no point continuing.
- **Fatal for one connection** (`readline`, `writen` failures): log, close,
  child exits 1. The parent is unaffected.
- **Ignorable / retryable** (`EINTR`, `ECONNABORTED`, `EPROTO`, transient
  `EMFILE`/`ENOBUFS`): keep going. A single misbehaving client must never
  take down the listener.

## 4. Control flow

```
main
 ├─ parse_port(argv[1])                strict, rejects "80x" and out-of-range
 ├─ install_parent_handlers()          SIGCHLD, SIGINT, SIGTERM, SIGPIPE
 ├─ socket / setsockopt / bind / listen
 └─ while (!shutdown_requested)
      ├─ accept()
      │    ├─ EINTR / ECONNABORTED / EPROTO  → continue
      │    ├─ EMFILE / ENOBUFS               → log, sleep 1, continue
      │    └─ other                          → die_sys
      └─ fork()
           ├─ child:  close(listenfd); reset_child_handlers();
           │          echo_service(connfd); close(connfd); _exit(status)
           └─ parent: close(connfd); loop
```

`echo_service()` is the whole protocol:

```
readline_reset()
loop:
    n = readline(connfd, buf, ECHO_MAXLINE + 1)
    n <  0  →  log error, return 1
    n == 0  →  EOF, log, return 0
    n >  0  →  writen(connfd, buf, n); log; continue
```

## 5. What could go wrong

| Hazard | Where it is dealt with |
|---|---|
| Short `write()` on a socket | `writen()` loops until all bytes are out |
| Line split across segments | `readline()` reads until `'\n'` |
| Signal interrupts a slow call | `EINTR` retried in `readline`, `writen`, `accept` |
| Zombie children | `SIGCHLD` + `waitpid(WNOHANG)` loop |
| Client vanishes mid-write | `SIGPIPE` ignored → `EPIPE` returned and logged |
| Restart inside `TIME_WAIT` | `SO_REUSEADDR` |
| Descriptor leak | Parent closes `connfd`, child closes `listenfd`; verified with `/proc/<pid>/fd` |
| Buffer overrun | `readline()` never writes past `maxlen - 1`; `writen()` uses the returned length, never `strlen()` (so embedded NULs are safe) |
