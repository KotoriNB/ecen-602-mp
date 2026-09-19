# MP1 — Test strategy

Submission Guideline 2 asks for a set of test cases, a short report describing
them, and **screen captures documenting correct operation**. This file is the
strategy; the report with the captures is what gets submitted alongside it.

## Strategy in one paragraph

Test in three layers. **Layer 1** is the server alone, driven by a raw-socket
harness (`tests/run_tests.py`) — this is where the awkward cases live, because
a script can send exactly 4096 bytes with no newline, or reset a connection
mid-line, which you cannot do reliably by typing at a terminal. **Layer 2** is
the real `echo` client against the real `echos` server, run by hand: this is
what the screen captures show, and it is the only layer that exercises the
client's stdin/stdout path. **Layer 3** is interoperability — our client
against the course reference server, and against another team's implementation.
Layer 1 catches bugs fast during development; layer 2 produces the evidence for
the report; layer 3 catches assumptions both halves of our own team made
together.

## Layer 1 — automated harness

```sh
make test                              # or:
./tests/run_tests.py --server ./echos -v
```

Starts `echos` on a free port, runs 11 cases, shuts it down with `SIGINT`,
exits non-zero if anything failed. Run it after every change; it takes a few
seconds.

| # | Case | What it proves |
|---|---|---|
| 1 | Line terminated by newline | **Required case 1.** Basic round trip |
| 1b | Four lines on one connection | Order preserved, connection stays up between lines |
| 2 | Exactly `ECHO_MAXLINE` bytes, no newline | **Required case 2.** `readline()` returns on the length limit and does not hang waiting for a `'\n'` |
| 2b | 6152 bytes in one line | Over-long line echoed in pieces with nothing lost at the chunk boundary |
| 3 | Connect, send nothing, close; then a bare `"\n"` | **Required case 3.** `readline()` returns 0 at EOF; an empty *line* still echoes one byte |
| 4 | Send text, then close with `SO_LINGER`=0 (RST) | **Required case 4.** Server survives a killed client; next connection still works |
| 5 | Three sockets open at once, interleaved writes, then reverse-order reads | **Required case 5.** Real concurrency; no cross-talk between the per-child `readline()` buffers |
| 6 | 12 connections, then `ps` for `<defunct>` | `SIGCHLD` handler reaps — no zombies |
| 7 | One line sent as five segments 150 ms apart | `readline()` reassembles a stream, does not assume one `read()` per line |
| 8 | Payload with `0x00`, `0x80`, `0xff` | `writen()` uses the byte count, not `strlen()` |
| 9 | 40 rapid sequential connections | `accept`/`fork` loop is stable under churn |

Last run: **11/11 passed**, clean `SIGINT` shutdown, 0 zombies, parent holding
4 descriptors (stdin/stdout/stderr + listening socket — no leak), and
`valgrind --leak-check=full --trace-children=yes` reporting **0 errors** in the
parent and in every child.

## Layer 2 — manual, for the screen captures

Two terminals. Left: the server. Right: the client(s). Capture **both**, since
the server log is the evidence for concurrency and EOF handling.

```sh
# terminal 1
./echos 9001

# terminal 2
./echo 127.0.0.1 9001
```

| Required case | How to drive it | What the capture must show |
|---|---|---|
| 1. Line + newline | Type `hello world` and press Enter | Client prints `hello world`; server logs `echoed 12 bytes` |
| 2. Max-length line, no newline | `python3 -c "import sys; sys.stdout.write('A'*4096)" \| ./echo 127.0.0.1 9001` | 4096 `A`s come back; server log notes the line-limit case |
| 3. No characters + EOF | Press Ctrl-D as the very first keystroke | Client exits immediately; server logs `EOF from client` then `connection closed` |
| 4. Client terminated after entering text | Type a line, get the echo, then `kill -9` the client from a third terminal | Server logs the error or EOF and the **parent keeps running**; a new client connects fine right after |
| 5. Three clients | `./tests/three_clients.sh 127.0.0.1 9001`, or three terminals each running `./echo` | Server log shows **three different child pids** alive at the same time, each echoing its own text |

Sample server log from the three-client run (this is what case 5 should look
like):

```
[2026-09-19 11:08:22] echos: listening on 0.0.0.0:9001 (pid 699)
[2026-09-19 11:08:23] 127.0.0.1:33552: accepted, handed to child pid 712
[2026-09-19 11:08:23] 127.0.0.1:33552: connection established (child pid 712)
[2026-09-19 11:08:23] 127.0.0.1:33552: echoed 16 bytes
[2026-09-19 11:08:23] 127.0.0.1:33554: accepted, handed to child pid 719
[2026-09-19 11:08:23] 127.0.0.1:33554: echoed 16 bytes
[2026-09-19 11:08:23] 127.0.0.1:33558: accepted, handed to child pid 726
[2026-09-19 11:08:23] 127.0.0.1:33558: echoed 16 bytes
[2026-09-19 11:08:25] 127.0.0.1:33552: EOF from client
[2026-09-19 11:08:25] 127.0.0.1:33552: connection closed (child pid 712 exiting)
...
```

Three distinct child pids, overlapping in time, is the proof of "multiple
simultaneous connections" — one screenshot of this is worth more than a
paragraph of prose in the report.

Save captures as `docs/screenshots/caseN-*.png`.

## Layer 3 — interoperability

1. **Our client against the course reference server** (Note 7 of the handout):
   ```sh
   ./echo 128.194.x.x 9001        # ecewkgsw05201.engr.tamu.edu port 9001
   ```
   Must be run from the campus network or over VPN. This validates the client
   independently of our own server, so a shared misunderstanding between the
   two halves of our team cannot hide.
2. **Our server against another team's client, and vice versa** (Note 6). The
   things this reliably breaks: line-length assumptions, whether the newline
   is echoed back, and whether either side waits for a newline that never
   comes.
3. **`nc` as a deliberately dumb client**: `nc 127.0.0.1 9001`. It does no
   framing at all, which is exactly why it is useful — anything that only
   works with our own client is a bug in our protocol assumptions.

## Things worth checking that are not in the required five

- **Zombies**: `ps -ef | grep defunct` after a dozen connections. Automated as
  case 6.
- **Descriptor leaks**: `ls /proc/$(pgrep -x echos)/fd | wc -l` after many
  connections — should stay at 4.
- **Memory**: `valgrind --leak-check=full --trace-children=yes ./echos 9001`.
- **Argument validation**: `./echos`, `./echos 0`, `./echos 99999`,
  `./echos 80x` — all rejected with a message, exit status 1.
- **Port already in use**: second `./echos 9001` must fail with
  `bind to port 9001: Address already in use`, not crash.
- **Graceful shutdown**: Ctrl-C the server; it closes the listening socket and
  exits rather than dying on the default handler.

## Before submitting

- [ ] `make clean` — Submission Guideline 6 (no binaries or `.o` files)
- [ ] `make` from a fresh copy on a standard Linux box — Guideline 7
- [ ] `make test` passes 11/11
- [ ] README names both team members and their roles — Guideline 1
- [ ] `docs/screenshots/` has a capture for each of the five required cases
- [ ] `docs/DESIGN.md` is included — Note 2 requires the design sketch
