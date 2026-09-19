/*
 * tests/test_echos.c -- automated test harness for the ECEN 602 MP1 echo server.
 *
 * This is a DEVELOPMENT tool, not part of the graded deliverable.  It speaks
 * raw TCP to the server so that each required test case can be driven exactly
 * and repeatably -- including the awkward ones (a line at the length limit
 * with no newline, a connection reset mid-line, three clients at once) that
 * are hard to trigger by hand at a terminal.
 *
 * It starts its own copy of the server on a free port, runs every case, then
 * shuts the server down with SIGINT (which also tests graceful shutdown).
 *
 * Usage:
 *     ./tests/test_echos [-v] [--server PATH] [--port PORT]
 *
 *   -v              let the server's log through to the terminal
 *   --server PATH   server binary to test (default ./echos)
 *   --port PORT     fixed port (default: ask the kernel for a free one)
 *
 * Exit status is 0 only if every case passed.
 *
 * ECEN 602 -- Machine Problem 1
 */

#include "echo_io.h"          /* ECHO_MAXLINE / ECHO_BUFSIZE stay in sync */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define IO_TIMEOUT_SEC  5           /* per-recv timeout, so a bug cannot hang */
#define DEFAULT_SERVER  "./echos"
#define SCRATCH_MAX     (ECHO_MAXLINE * 2 + 64)

static int tests_passed;
static int tests_failed;

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

static void pass(const char *name, const char *fmt, ...)
{
    va_list ap;

    printf("  PASS  %s", name);
    if (fmt != NULL && *fmt != '\0') {
        printf("  [");
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("]");
    }
    printf("\n");
    fflush(stdout);
    tests_passed++;
}

static void fail(const char *name, const char *fmt, ...)
{
    va_list ap;

    printf("  FAIL  %s  [", name);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("]\n");
    fflush(stdout);
    tests_failed++;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void msleep(long millis)
{
    struct timespec ts;

    ts.tv_sec  = millis / 1000;
    ts.tv_nsec = (millis % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

/*
 * Ask the kernel for an unused port: bind to port 0, read back what we got,
 * then release it.  A race is theoretically possible but the window is tiny
 * and this beats hard-coding a port that may already be busy.
 */
static uint16_t free_port(void)
{
    struct sockaddr_in addr;
    socklen_t          len = sizeof(addr);
    uint16_t           port = 0;
    int                fd;

    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return 0;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
        getsockname(fd, (struct sockaddr *)&addr, &len) == 0)
        port = ntohs(addr.sin_port);

    close(fd);
    return port;
}

/* Connect to the server under test.  Timeouts keep a hung server from
 * hanging the harness. */
static int connect_server(uint16_t port)
{
    struct sockaddr_in addr;
    struct timeval     tv;
    int                fd;

    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;

    tv.tv_sec  = IO_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(port);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Write all n bytes, retrying on EINTR.  Returns 0 or -1. */
static int send_all(int fd, const void *vptr, size_t n)
{
    const char *ptr   = vptr;
    size_t      nleft = n;

    while (nleft > 0) {
        ssize_t nw = write(fd, ptr, nleft);

        if (nw <= 0) {
            if (nw < 0 && errno == EINTR)
                continue;
            return -1;
        }
        nleft -= (size_t)nw;
        ptr   += nw;
    }
    return 0;
}

/* Read exactly n bytes.  Returns 0, or -1 on EOF / timeout / error. */
static int recv_exactly(int fd, void *vptr, size_t n)
{
    char  *ptr = vptr;
    size_t got = 0;

    while (got < n) {
        ssize_t nr = read(fd, ptr + got, n - got);

        if (nr < 0) {
            if (errno == EINTR)
                continue;
            return -1;                  /* includes EAGAIN from the timeout */
        }
        if (nr == 0)
            return -1;                  /* peer closed early */
        got += (size_t)nr;
    }
    return 0;
}

/* Read until EOF or timeout; returns the byte count (capped at cap). */
static size_t recv_drain(int fd, char *buf, size_t cap)
{
    size_t got = 0;

    while (got < cap) {
        ssize_t nr = read(fd, buf + got, cap - got);

        if (nr < 0) {
            if (errno == EINTR)
                continue;
            break;                      /* timeout: treat as end of data */
        }
        if (nr == 0)
            break;
        got += (size_t)nr;
    }
    return got;
}

/* Count zombie (<defunct>) echos processes via ps. */
static int count_zombie_servers(void)
{
    char  line[256];
    FILE *fp;
    int   count = 0;

    if ((fp = popen("ps -e -o stat=,comm=", "r")) == NULL)
        return -1;

    while (fgets(line, sizeof(line), fp) != NULL) {
        const char *p = line;

        while (*p == ' ')
            p++;
        if (*p == 'Z' && strstr(line, "echos") != NULL)
            count++;
    }

    pclose(fp);
    return count;
}

/* ------------------------------------------------------------------ */
/* Server lifecycle                                                    */
/* ------------------------------------------------------------------ */

static pid_t spawn_server(const char *path, uint16_t port, int verbose)
{
    char  portbuf[16];
    pid_t pid;

    snprintf(portbuf, sizeof(portbuf), "%u", (unsigned)port);

    if ((pid = fork()) < 0)
        return -1;

    if (pid == 0) {
        if (!verbose) {
            int devnull = open("/dev/null", O_WRONLY);

            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > STDERR_FILENO)
                    close(devnull);
            }
        }
        execl(path, path, portbuf, (char *)NULL);
        _exit(127);                     /* exec failed */
    }
    return pid;
}

/* Poll until the server is accepting, or give up after ~5 seconds. */
static int wait_for_listen(uint16_t port, pid_t server)
{
    int attempt;

    for (attempt = 0; attempt < 100; attempt++) {
        int fd = connect_server(port);

        if (fd >= 0) {
            close(fd);
            return 0;
        }
        if (waitpid(server, NULL, WNOHANG) == server)
            return -1;                  /* server died during startup */
        msleep(50);
    }
    return -1;
}

static int server_alive(pid_t pid)
{
    return waitpid(pid, NULL, WNOHANG) == 0;
}

/* ------------------------------------------------------------------ */
/* Required test cases (Submission Guideline 2)                        */
/* ------------------------------------------------------------------ */

/* (1) A line of text terminated by a newline. */
static void case_line_with_newline(uint16_t port)
{
    static const char name[] = "1. line terminated by newline";
    const char        payload[] = "Hello, ECEN 602!\n";
    size_t            len = strlen(payload);
    char              got[64];
    int               fd;

    if ((fd = connect_server(port)) < 0) {
        fail(name, "connect: %s", strerror(errno));
        return;
    }
    if (send_all(fd, payload, len) < 0) {
        fail(name, "send: %s", strerror(errno));
        close(fd);
        return;
    }
    if (recv_exactly(fd, got, len) < 0) {
        fail(name, "short read: %s", strerror(errno));
        close(fd);
        return;
    }
    if (memcmp(got, payload, len) != 0)
        fail(name, "payload mismatch");
    else
        pass(name, "%zu bytes round-tripped", len);

    close(fd);
}

/* (1b) Several lines on one connection, order preserved. */
static void case_multiple_lines(uint16_t port)
{
    static const char  name[] = "1b. multiple lines, one connection";
    static const char *lines[] = { "first\n", "second\n", "\n", "fourth line\n" };
    const size_t       count = sizeof(lines) / sizeof(lines[0]);
    char               got[64];
    size_t             i;
    int                fd;

    if ((fd = connect_server(port)) < 0) {
        fail(name, "connect: %s", strerror(errno));
        return;
    }

    for (i = 0; i < count; i++) {
        size_t len = strlen(lines[i]);

        if (send_all(fd, lines[i], len) < 0 ||
            recv_exactly(fd, got, len) < 0 ||
            memcmp(got, lines[i], len) != 0) {
            fail(name, "line %zu did not round-trip", i + 1);
            close(fd);
            return;
        }
    }

    pass(name, "%zu lines in order", count);
    close(fd);
}

/* (2) A line of the maximum line length with no newline. */
static void case_maxline_no_newline(uint16_t port)
{
    static const char name[] = "2. max-length line, no newline";
    char             *sent = malloc(ECHO_MAXLINE);
    char             *got  = malloc(ECHO_MAXLINE);
    char              extra[64];
    int               fd;

    if (sent == NULL || got == NULL) {
        fail(name, "out of memory");
        free(sent);
        free(got);
        return;
    }
    memset(sent, 'A', ECHO_MAXLINE);

    if ((fd = connect_server(port)) < 0) {
        fail(name, "connect: %s", strerror(errno));
        free(sent);
        free(got);
        return;
    }

    if (send_all(fd, sent, ECHO_MAXLINE) < 0) {
        fail(name, "send: %s", strerror(errno));
    } else if (recv_exactly(fd, got, ECHO_MAXLINE) < 0) {
        fail(name, "server did not return %d bytes (it may be waiting for a "
                   "newline)", ECHO_MAXLINE);
    } else if (memcmp(sent, got, ECHO_MAXLINE) != 0) {
        fail(name, "payload mismatch");
    } else {
        size_t trailing;

        shutdown(fd, SHUT_WR);
        trailing = recv_drain(fd, extra, sizeof(extra));
        if (trailing != 0)
            fail(name, "%zu unexpected extra bytes", trailing);
        else
            pass(name, "%d bytes echoed without a newline", ECHO_MAXLINE);
    }

    close(fd);
    free(sent);
    free(got);
}

/* (2b) A line LONGER than the buffer: must come back whole, in pieces. */
static void case_over_maxline(uint16_t port)
{
    static const char name[] = "2b. line longer than the buffer";
    const size_t      n = ECHO_MAXLINE + ECHO_MAXLINE / 2 + 7;
    char             *sent = malloc(n + 1);
    char             *got  = malloc(n + 1);
    size_t            i;
    int               fd;

    if (sent == NULL || got == NULL) {
        fail(name, "out of memory");
        free(sent);
        free(got);
        return;
    }
    for (i = 0; i < n; i++)
        sent[i] = (char)('A' + (i % 26));
    sent[n] = '\n';

    if ((fd = connect_server(port)) < 0) {
        fail(name, "connect: %s", strerror(errno));
        free(sent);
        free(got);
        return;
    }

    if (send_all(fd, sent, n + 1) < 0)
        fail(name, "send: %s", strerror(errno));
    else if (recv_exactly(fd, got, n + 1) < 0)
        fail(name, "short read: %s", strerror(errno));
    else if (memcmp(sent, got, n + 1) != 0)
        fail(name, "mismatch across a chunk boundary");
    else
        pass(name, "%zu bytes preserved across reads", n + 1);

    close(fd);
    free(sent);
    free(got);
}

/* (3) A line with no characters, followed by EOF. */
static void case_empty_then_eof(uint16_t port)
{
    static const char name[] = "3. no characters, then EOF";
    char              buf[64];
    size_t            leftover;
    int               fd;

    /* 3a: connect and close immediately, sending nothing at all. */
    if ((fd = connect_server(port)) < 0) {
        fail(name, "connect: %s", strerror(errno));
        return;
    }
    shutdown(fd, SHUT_WR);
    leftover = recv_drain(fd, buf, sizeof(buf));
    close(fd);

    if (leftover != 0) {
        fail(name, "server sent %zu bytes for an empty stream", leftover);
        return;
    }

    /* 3b: a bare newline is an empty *line* and must still echo. */
    if ((fd = connect_server(port)) < 0) {
        fail(name, "reconnect: %s", strerror(errno));
        return;
    }
    if (send_all(fd, "\n", 1) < 0 || recv_exactly(fd, buf, 1) < 0)
        fail(name, "bare newline did not round-trip");
    else if (buf[0] != '\n')
        fail(name, "bare newline echoed as 0x%02x", (unsigned char)buf[0]);
    else
        pass(name, "empty stream closes cleanly; bare newline echoes");

    close(fd);
}

/* (4) Client terminated after entering text (abrupt close -> RST). */
static void case_client_killed(uint16_t port)
{
    static const char name[] = "4. client killed after sending text";
    const char        payload[] = "text before the client dies\n";
    const char        probe[]   = "still alive\n";
    size_t            len = strlen(payload);
    struct linger     lg;
    char              got[64];
    int               fd;

    if ((fd = connect_server(port)) < 0) {
        fail(name, "connect: %s", strerror(errno));
        return;
    }
    if (send_all(fd, payload, len) < 0 || recv_exactly(fd, got, len) < 0) {
        fail(name, "first exchange failed");
        close(fd);
        return;
    }

    /* SO_LINGER with a zero timeout makes close() send RST instead of FIN --
     * which is what a SIGKILLed client looks like to the server. */
    lg.l_onoff  = 1;
    lg.l_linger = 0;
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    close(fd);
    msleep(300);

    /* The server must still be listening and serving. */
    if ((fd = connect_server(port)) < 0) {
        fail(name, "server stopped accepting after the reset");
        return;
    }
    len = strlen(probe);
    if (send_all(fd, probe, len) < 0 || recv_exactly(fd, got, len) < 0 ||
        memcmp(got, probe, len) != 0)
        fail(name, "server unhealthy after client reset");
    else
        pass(name, "RST handled, server still serving");

    close(fd);
}

/* (5) Three clients connected to the server simultaneously. */
static void case_three_clients(uint16_t port)
{
    static const char name[] = "5. three simultaneous clients";
    int               fd[3];
    char              want[64], got[64];
    int               i;

    for (i = 0; i < 3; i++) {
        if ((fd[i] = connect_server(port)) < 0) {
            fail(name, "client %d could not connect", i);
            while (--i >= 0)
                close(fd[i]);
            return;
        }
    }

    /* Every client sends before any client reads, so all three connections
     * really are open and in flight at the same time. */
    for (i = 0; i < 3; i++) {
        snprintf(want, sizeof(want), "client-%d round-one\n", i);
        if (send_all(fd[i], want, strlen(want)) < 0) {
            fail(name, "client %d send failed", i);
            goto cleanup;
        }
    }
    for (i = 0; i < 3; i++) {
        size_t len;

        snprintf(want, sizeof(want), "client-%d round-one\n", i);
        len = strlen(want);
        if (recv_exactly(fd[i], got, len) < 0 || memcmp(got, want, len) != 0) {
            fail(name, "client %d got the wrong reply", i);
            goto cleanup;
        }
    }

    /* Second round in reverse order: proves the per-connection readline()
     * buffers are not shared between children. */
    for (i = 2; i >= 0; i--) {
        snprintf(want, sizeof(want), "client-%d round-two\n", i);
        if (send_all(fd[i], want, strlen(want)) < 0) {
            fail(name, "client %d second send failed", i);
            goto cleanup;
        }
    }
    for (i = 0; i < 3; i++) {
        size_t len;

        snprintf(want, sizeof(want), "client-%d round-two\n", i);
        len = strlen(want);
        if (recv_exactly(fd[i], got, len) < 0 || memcmp(got, want, len) != 0) {
            fail(name, "client %d crossed streams", i);
            goto cleanup;
        }
    }

    pass(name, "3 concurrent connections, no cross-talk");

cleanup:
    for (i = 0; i < 3; i++)
        close(fd[i]);
}

/* ------------------------------------------------------------------ */
/* Extra cases: things the handout warns about                         */
/* ------------------------------------------------------------------ */

/* Children must be reaped: no <defunct> processes after churn. */
static void case_no_zombies(uint16_t port)
{
    static const char name[] = "6. no zombie children after 12 connections";
    char              got[16];
    int               i, zombies;

    for (i = 0; i < 12; i++) {
        int fd = connect_server(port);

        if (fd < 0) {
            fail(name, "connection %d failed", i);
            return;
        }
        if (send_all(fd, "churn\n", 6) < 0 || recv_exactly(fd, got, 6) < 0) {
            fail(name, "connection %d did not echo", i);
            close(fd);
            return;
        }
        close(fd);
    }

    msleep(500);
    zombies = count_zombie_servers();

    if (zombies < 0)
        fail(name, "could not run ps");
    else if (zombies > 0)
        fail(name, "%d zombie echos process(es) left behind", zombies);
    else
        pass(name, "SIGCHLD handler reaped every child");
}

/* A line delivered in several TCP segments must echo once, whole. */
static void case_split_segments(uint16_t port)
{
    static const char  name[] = "7. line split across TCP segments";
    static const char *pieces[] = { "slow", "ly ", "assembled", " line" };
    const char         want[] = "slowly assembled line\n";
    size_t             len = strlen(want);
    char               got[64];
    size_t             i;
    int                fd;

    if ((fd = connect_server(port)) < 0) {
        fail(name, "connect: %s", strerror(errno));
        return;
    }

    for (i = 0; i < sizeof(pieces) / sizeof(pieces[0]); i++) {
        if (send_all(fd, pieces[i], strlen(pieces[i])) < 0) {
            fail(name, "send piece %zu failed", i);
            close(fd);
            return;
        }
        msleep(150);
    }
    if (send_all(fd, "\n", 1) < 0) {
        fail(name, "send newline failed");
        close(fd);
        return;
    }

    if (recv_exactly(fd, got, len) < 0 || memcmp(got, want, len) != 0)
        fail(name, "reassembly failed");
    else
        pass(name, "readline() waited for the newline");

    close(fd);
}

/* Bytes above 0x7f and embedded NULs must survive the round trip. */
static void case_binary_safe(uint16_t port)
{
    static const char name[] = "8. 8-bit clean (NUL and high bytes)";
    const char        payload[] = { 0x00, 0x01, (char)0xC8, (char)0xFF,
                                    'A', 0x00, (char)0x80, '\n' };
    const size_t      len = sizeof(payload);
    char              got[16];
    int               fd;

    if ((fd = connect_server(port)) < 0) {
        fail(name, "connect: %s", strerror(errno));
        return;
    }

    if (send_all(fd, payload, len) < 0 || recv_exactly(fd, got, len) < 0)
        fail(name, "exchange failed");
    else if (memcmp(got, payload, len) != 0)
        fail(name, "payload mangled -- writen() may be using strlen()");
    else
        pass(name, "writen() used the length, not strlen()");

    close(fd);
}

/* Many short-lived connections back to back (fork/accept stress). */
static void case_rapid_connections(uint16_t port)
{
    static const char name[] = "9. 40 rapid sequential connections";
    char              msg[32], got[32];
    int               i;

    for (i = 0; i < 40; i++) {
        int    fd = connect_server(port);
        size_t len;

        if (fd < 0) {
            fail(name, "connection %d could not connect", i);
            return;
        }
        snprintf(msg, sizeof(msg), "n=%d\n", i);
        len = strlen(msg);

        if (send_all(fd, msg, len) < 0 || recv_exactly(fd, got, len) < 0 ||
            memcmp(got, msg, len) != 0) {
            fail(name, "connection %d mismatch", i);
            close(fd);
            return;
        }
        close(fd);
    }

    pass(name, "accept/fork loop stable");
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

typedef void (*test_fn)(uint16_t);

static const struct {
    const char *label;
    test_fn     fn;
} CASES[] = {
    { "1",  case_line_with_newline   },
    { "1b", case_multiple_lines      },
    { "2",  case_maxline_no_newline  },
    { "2b", case_over_maxline        },
    { "3",  case_empty_then_eof      },
    { "4",  case_client_killed       },
    { "5",  case_three_clients       },
    { "6",  case_no_zombies          },
    { "7",  case_split_segments      },
    { "8",  case_binary_safe         },
    { "9",  case_rapid_connections   },
};

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-v] [--server PATH] [--port PORT]\n", prog);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    const char *server_path = DEFAULT_SERVER;
    uint16_t    port = 0;
    int         verbose = 0;
    pid_t       server;
    size_t      i;
    int         status, total;

    for (i = 1; (int)i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--server") == 0 && (int)i + 1 < argc) {
            server_path = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && (int)i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else {
            usage(argv[0]);
        }
    }

    /* A test that writes to a socket the server has closed must see EPIPE,
     * not die. */
    signal(SIGPIPE, SIG_IGN);

    if (access(server_path, X_OK) < 0) {
        fprintf(stderr, "error: %s not found or not executable -- run 'make' "
                        "first\n", server_path);
        return 2;
    }

    if (port == 0 && (port = free_port()) == 0) {
        fprintf(stderr, "error: could not find a free port\n");
        return 2;
    }

    printf("starting %s on port %u\n", server_path, (unsigned)port);

    if ((server = spawn_server(server_path, port, verbose)) < 0) {
        fprintf(stderr, "error: fork failed: %s\n", strerror(errno));
        return 2;
    }
    if (wait_for_listen(port, server) < 0) {
        fprintf(stderr, "error: server never started listening\n");
        kill(server, SIGKILL);
        waitpid(server, NULL, 0);
        return 2;
    }

    printf("\n");
    for (i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        CASES[i].fn(port);
        if (!server_alive(server)) {
            fprintf(stderr, "  !! server died mid-run\n");
            server = -1;
            break;
        }
    }
    printf("\n");

    /* Shutting down with SIGINT also exercises the graceful-shutdown path. */
    if (server > 0) {
        int waited;

        kill(server, SIGINT);
        for (waited = 0; waited < 30; waited++) {
            if (waitpid(server, &status, WNOHANG) == server)
                break;
            msleep(100);
        }
        if (waited == 30) {
            fail("graceful shutdown on SIGINT", "server ignored SIGINT");
            kill(server, SIGKILL);
            waitpid(server, NULL, 0);
        } else {
            printf("server shut down cleanly on SIGINT\n");
        }
    }

    total = tests_passed + tests_failed;
    printf("\n%d/%d cases passed\n", tests_passed, total);

    return (tests_failed == 0) ? 0 : 1;
}
