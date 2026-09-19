#!/usr/bin/env python3
"""
run_tests.py -- automated test harness for the ECEN 602 MP1 echo server.

This is a *development* tool, not part of the graded C deliverable.  It
speaks raw TCP to the server so that each of the required test cases can be
driven exactly and repeatably -- including the awkward ones (a client that
dies mid-line, a line with no terminating newline, three clients at once)
that are hard to trigger by hand at a terminal.

Usage:
    ./tests/run_tests.py --server ./echos [--port 0] [--maxline 4096] [-v]

Exit status is 0 only if every case passes.
"""

import argparse
import os
import signal
import socket
import subprocess
import sys
import time

TIMEOUT = 5.0


class Result:
    def __init__(self):
        self.passed = []
        self.failed = []

    def ok(self, name, detail=""):
        self.passed.append(name)
        print(f"  PASS  {name}" + (f"  [{detail}]" if detail else ""))

    def bad(self, name, detail):
        self.failed.append((name, detail))
        print(f"  FAIL  {name}  [{detail}]")


def free_port():
    """Ask the kernel for an unused port, then release it."""
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def connect(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=TIMEOUT)
    s.settimeout(TIMEOUT)
    return s


def recv_exactly(sock, n):
    """Read exactly n bytes, or raise on EOF/timeout."""
    chunks = []
    got = 0
    while got < n:
        b = sock.recv(min(65536, n - got))
        if not b:
            raise EOFError(f"peer closed after {got} of {n} bytes")
        chunks.append(b)
        got += len(b)
    return b"".join(chunks)


def recv_until_eof(sock, limit=1 << 20):
    chunks = []
    total = 0
    while total < limit:
        try:
            b = sock.recv(65536)
        except socket.timeout:
            break
        if not b:
            break
        chunks.append(b)
        total += len(b)
    return b"".join(chunks)


# --------------------------------------------------------------------------
# Required test cases (Submission Guideline 2)
# --------------------------------------------------------------------------

def case1_line_with_newline(port, r, _maxline):
    """(1) A line of text terminated by a newline."""
    name = "1. line terminated by newline"
    try:
        with connect(port) as s:
            payload = b"Hello, ECEN 602!\n"
            s.sendall(payload)
            echoed = recv_exactly(s, len(payload))
            if echoed != payload:
                return r.bad(name, f"got {echoed!r}")
        r.ok(name, f"{len(payload)} bytes round-tripped")
    except Exception as e:
        r.bad(name, repr(e))


def case1b_multiple_lines(port, r, _maxline):
    """(1b) Several lines on one connection, order preserved."""
    name = "1b. multiple lines, one connection"
    try:
        with connect(port) as s:
            lines = [b"first\n", b"second\n", b"\n", b"fourth line\n"]
            for ln in lines:
                s.sendall(ln)
                got = recv_exactly(s, len(ln))
                if got != ln:
                    return r.bad(name, f"sent {ln!r} got {got!r}")
        r.ok(name, f"{len(lines)} lines in order")
    except Exception as e:
        r.bad(name, repr(e))


def case2_maxline_no_newline(port, r, maxline):
    """(2) A line of the maximum line length with no newline."""
    name = "2. max-length line, no newline"
    try:
        with connect(port) as s:
            payload = b"A" * maxline            # exactly MAXLINE, no '\n'
            s.sendall(payload)
            echoed = recv_exactly(s, maxline)
            if echoed != payload:
                return r.bad(name, "payload mismatch")
            s.shutdown(socket.SHUT_WR)
            trailing = recv_until_eof(s)
            if trailing:
                return r.bad(name, f"unexpected extra {len(trailing)} bytes")
        r.ok(name, f"{maxline} bytes echoed without a newline")
    except Exception as e:
        r.bad(name, repr(e))


def case2b_over_maxline(port, r, maxline):
    """(2b) A line LONGER than the buffer: must come back whole, in pieces."""
    name = "2b. line longer than the buffer"
    n = maxline + maxline // 2 + 7
    try:
        with connect(port) as s:
            payload = bytes((i % 26) + 65 for i in range(n)) + b"\n"
            s.sendall(payload)
            echoed = recv_exactly(s, len(payload))
            if echoed != payload:
                return r.bad(name, "payload mismatch across chunk boundary")
        r.ok(name, f"{n + 1} bytes preserved across reads")
    except Exception as e:
        r.bad(name, repr(e))


def case3_empty_then_eof(port, r, _maxline):
    """(3) A line with no characters, followed by EOF."""
    name = "3. no characters, then EOF"
    try:
        # 3a: connect and close immediately, sending nothing at all.
        s = connect(port)
        s.shutdown(socket.SHUT_WR)
        leftover = recv_until_eof(s)
        s.close()
        if leftover:
            return r.bad(name, f"server sent {leftover!r} for an empty stream")

        # 3b: a bare newline (an empty *line*) must still echo.
        with connect(port) as s2:
            s2.sendall(b"\n")
            got = recv_exactly(s2, 1)
            if got != b"\n":
                return r.bad(name, f"bare newline echoed as {got!r}")
            s2.shutdown(socket.SHUT_WR)
        r.ok(name, "empty stream closes cleanly; bare newline echoes")
    except Exception as e:
        r.bad(name, repr(e))


def case4_client_terminated(port, r, _maxline):
    """(4) Client terminated after entering text (abrupt close / RST)."""
    name = "4. client killed after sending text"
    try:
        s = connect(port)
        s.sendall(b"text before the client dies\n")
        recv_exactly(s, len(b"text before the client dies\n"))
        # Hard reset: SO_LINGER with timeout 0 makes close() send RST, which
        # is what a killed client looks like to the server.
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                     b"\x01\x00\x00\x00\x00\x00\x00\x00")
        s.close()
        time.sleep(0.3)

        # The server must still be alive and accepting.
        with connect(port) as s2:
            s2.sendall(b"still alive\n")
            if recv_exactly(s2, 12) != b"still alive\n":
                return r.bad(name, "server unhealthy after client reset")
        r.ok(name, "RST handled, server still serving")
    except Exception as e:
        r.bad(name, repr(e))


def case5_three_clients(port, r, _maxline):
    """(5) Three clients connected to the server simultaneously."""
    name = "5. three simultaneous clients"
    socks = []
    try:
        for _ in range(3):
            socks.append(connect(port))

        # Interleave: every client sends before any client reads, so the
        # three connections really are open and in flight at the same time.
        for i, s in enumerate(socks):
            s.sendall(f"client-{i} round-one\n".encode())
        for i, s in enumerate(socks):
            want = f"client-{i} round-one\n".encode()
            got = recv_exactly(s, len(want))
            if got != want:
                return r.bad(name, f"client {i} got {got!r}")

        # Second round in reverse order, to prove no cross-talk between the
        # per-connection readline() buffers.
        for i, s in reversed(list(enumerate(socks))):
            s.sendall(f"client-{i} round-two\n".encode())
        for i, s in enumerate(socks):
            want = f"client-{i} round-two\n".encode()
            got = recv_exactly(s, len(want))
            if got != want:
                return r.bad(name, f"client {i} crossed streams: {got!r}")

        r.ok(name, "3 concurrent connections, no cross-talk")
    except Exception as e:
        r.bad(name, repr(e))
    finally:
        for s in socks:
            try:
                s.close()
            except OSError:
                pass


# --------------------------------------------------------------------------
# Extra cases: things the handout warns about
# --------------------------------------------------------------------------

def case6_no_zombies(port, r, _maxline):
    """Children must be reaped: no <defunct> processes after churn."""
    name = "6. no zombie children after 12 connections"
    try:
        for i in range(12):
            with connect(port) as s:
                s.sendall(b"churn\n")
                recv_exactly(s, 6)
        time.sleep(0.5)
        out = subprocess.run(["ps", "-e", "-o", "stat=,comm="],
                             capture_output=True, text=True).stdout
        zombies = [l for l in out.splitlines()
                   if l.strip().startswith("Z") and "echos" in l]
        if zombies:
            return r.bad(name, f"{len(zombies)} zombies: {zombies[:3]}")
        r.ok(name, "SIGCHLD handler reaped every child")
    except Exception as e:
        r.bad(name, repr(e))


def case7_partial_line(port, r, _maxline):
    """A line delivered in several TCP segments must echo once, whole."""
    name = "7. line split across TCP segments"
    try:
        with connect(port) as s:
            for piece in (b"slow", b"ly ", b"assembled", b" line"):
                s.sendall(piece)
                time.sleep(0.15)
            s.sendall(b"\n")
            want = b"slowly assembled line\n"
            got = recv_exactly(s, len(want))
            if got != want:
                return r.bad(name, f"got {got!r}")
        r.ok(name, "readline() waited for the newline")
    except Exception as e:
        r.bad(name, repr(e))


def case8_binary_safe(port, r, _maxline):
    """Bytes above 0x7f and embedded NULs must survive the round trip."""
    name = "8. 8-bit clean (NUL and high bytes)"
    try:
        with connect(port) as s:
            payload = bytes([0, 1, 200, 255, 65, 0, 128]) + b"\n"
            s.sendall(payload)
            got = recv_exactly(s, len(payload))
            if got != payload:
                return r.bad(name, f"got {got!r}")
        r.ok(name, "writen() used the length, not strlen()")
    except Exception as e:
        r.bad(name, repr(e))


def case9_rapid_sequential(port, r, _maxline):
    """Many short-lived connections back to back (fork/accept stress)."""
    name = "9. 40 rapid sequential connections"
    try:
        for i in range(40):
            with connect(port) as s:
                msg = f"n={i}\n".encode()
                s.sendall(msg)
                if recv_exactly(s, len(msg)) != msg:
                    return r.bad(name, f"connection {i} mismatch")
        r.ok(name, "accept/fork loop stable")
    except Exception as e:
        r.bad(name, repr(e))


CASES = [
    case1_line_with_newline,
    case1b_multiple_lines,
    case2_maxline_no_newline,
    case2b_over_maxline,
    case3_empty_then_eof,
    case4_client_terminated,
    case5_three_clients,
    case6_no_zombies,
    case7_partial_line,
    case8_binary_safe,
    case9_rapid_sequential,
]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--server", default="./echos", help="path to the echos binary")
    ap.add_argument("--port", type=int, default=0, help="0 = pick a free port")
    ap.add_argument("--maxline", type=int, default=4096,
                    help="must match ECHO_MAXLINE in echo_io.h")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="show the server's log on stdout")
    args = ap.parse_args()

    if not os.path.exists(args.server):
        print(f"error: {args.server} not found -- run 'make' first", file=sys.stderr)
        return 2

    port = args.port or free_port()
    sink = None if args.verbose else subprocess.DEVNULL

    print(f"starting {args.server} on port {port}")
    server = subprocess.Popen([args.server, str(port)],
                              stdout=sink, stderr=sink)

    # Wait for the listening socket to come up.
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            connect(port).close()
            break
        except OSError:
            if server.poll() is not None:
                print("error: server exited during startup", file=sys.stderr)
                return 2
            time.sleep(0.05)
    else:
        print("error: server never started listening", file=sys.stderr)
        server.kill()
        return 2

    r = Result()
    print()
    try:
        for case in CASES:
            case(port, r, args.maxline)
            if server.poll() is not None:
                print("  !! server died mid-run", file=sys.stderr)
                break
    finally:
        print()
        if server.poll() is None:
            server.send_signal(signal.SIGINT)   # exercises graceful shutdown
            try:
                server.wait(timeout=3)
                print("server shut down cleanly on SIGINT")
            except subprocess.TimeoutExpired:
                server.kill()
                print("server did not honour SIGINT; killed", file=sys.stderr)
                r.bad("graceful shutdown on SIGINT", "timed out")

    total = len(r.passed) + len(r.failed)
    print(f"\n{len(r.passed)}/{total} cases passed")
    return 0 if not r.failed else 1


if __name__ == "__main__":
    sys.exit(main())
