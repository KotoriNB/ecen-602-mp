/*
 * echos.c -- Concurrent TCP echo server (RFC 862) for ECEN 602 MP1.
 *
 * Usage:  ./echos <port>
 *
 * ARCHITECTURE
 * ------------
 *   parent:  socket() -> setsockopt(SO_REUSEADDR) -> bind() -> listen()
 *            for (;;) { accept() -> fork() -> close(connfd) }
 *
 *   child:   close(listenfd)
 *            for (;;) { readline() -> writen() }   until EOF or error
 *            close(connfd) -> _exit()
 *
 * One child process per connection is what gives us "multiple simultaneous
 * connections".  Because each client is served by a separate process there
 * is no shared state to protect, and a client that misbehaves can only take
 * down its own child.
 *
 * SIGNALS
 * -------
 *   SIGCHLD  handler reaps finished children with waitpid(WNOHANG) in a loop
 *            so that no zombies accumulate.  Installed with SA_RESTART so
 *            that a child exiting does not abort a blocked accept().
 *   SIGPIPE  ignored.  If a client vanishes mid-echo we want writen() to
 *            fail with EPIPE and let us log it, not to kill the child
 *            silently.
 *   SIGINT / SIGTERM
 *            handler sets a flag and is installed WITHOUT SA_RESTART, so a
 *            blocked accept() returns EINTR and the parent can shut the
 *            listening socket down cleanly (nice for Ctrl-C in a demo).
 *
 * ECEN 602 -- Machine Problem 1
 */

#include "echo_io.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Backlog for listen().  Deep enough that a burst of simultaneous connects
 * (the three-client test case, and then some) never gets refused. */
#define ECHO_LISTENQ 128

/* "255.255.255.255:65535" plus slack. */
#define PEERSTR_MAX 32

/* Set by the SIGINT/SIGTERM handler; polled by the accept() loop. */
static volatile sig_atomic_t shutdown_requested = 0;

/* ------------------------------------------------------------------ */
/* Signal handling                                                     */
/* ------------------------------------------------------------------ */

/*
 * Reap every child that has finished.  WNOHANG in a loop is required: on a
 * busy server several children can exit while one SIGCHLD is pending, and
 * standard signals do not queue.
 *
 * waitpid() can clobber errno, and we may be interrupting a main-line
 * system call that is about to inspect it, so save and restore it.
 */
static void sigchld_handler(int signo)
{
    int saved_errno = errno;

    (void)signo;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;

    errno = saved_errno;
}

static void shutdown_handler(int signo)
{
    (void)signo;
    shutdown_requested = 1;
}

/*
 * install_handler -- thin sigaction() wrapper.  The handout notes that the
 * POSIX way to deal with signals is sigaction(), not signal().
 */
static void install_handler(int signo, void (*handler)(int), int flags)
{
    struct sigaction act;

    memset(&act, 0, sizeof(act));
    act.sa_handler = handler;
    act.sa_flags   = flags;
    sigemptyset(&act.sa_mask);

    if (sigaction(signo, &act, NULL) < 0)
        die_sys("sigaction for signal %d", signo);
}

static void install_parent_handlers(void)
{
    /* SA_NOCLDSTOP: we only care about children that terminate.
     * SA_RESTART:   do not disturb a blocked accept() just because a child
     *               finished. */
    install_handler(SIGCHLD, sigchld_handler, SA_RESTART | SA_NOCLDSTOP);

    /* No SA_RESTART here: we WANT accept() to return EINTR so the loop can
     * notice shutdown_requested and exit cleanly. */
    install_handler(SIGINT,  shutdown_handler, 0);
    install_handler(SIGTERM, shutdown_handler, 0);

    /* Let writen() report EPIPE instead of the process dying. */
    install_handler(SIGPIPE, SIG_IGN, 0);
}

/*
 * A child inherits the parent's dispositions.  It has no children of its
 * own to reap and should simply die on Ctrl-C, so put those back to the
 * default.  SIGPIPE stays ignored -- that one matters most in the child.
 */
static void reset_child_handlers(void)
{
    install_handler(SIGCHLD, SIG_DFL, 0);
    install_handler(SIGINT,  SIG_DFL, 0);
    install_handler(SIGTERM, SIG_DFL, 0);
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * parse_port -- strict conversion of argv[1].  strtol() rather than atoi()
 * so that "80x", "" and out-of-range values are rejected instead of
 * silently turning into something else.
 */
static uint16_t parse_port(const char *s)
{
    char *end;
    long  value;

    errno = 0;
    value = strtol(s, &end, 10);

    if (end == s || *end != '\0' || errno != 0 || value < 1 || value > 65535)
        die_msg("invalid port \"%s\": expected an integer in 1-65535", s);

    return (uint16_t)value;
}

/* Render a peer address as "a.b.c.d:port" for the log. */
static void format_peer(const struct sockaddr_in *addr, char *out, size_t len)
{
    char ip[INET_ADDRSTRLEN];

    if (inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)) == NULL)
        snprintf(ip, sizeof(ip), "?.?.?.?");

    snprintf(out, len, "%s:%u", ip, (unsigned)ntohs(addr->sin_port));
}

/* ------------------------------------------------------------------ */
/* Per-connection service (runs in the child)                          */
/* ------------------------------------------------------------------ */

/*
 * echo_service -- read lines from the connected socket and write them
 * straight back, until the client closes its end.
 *
 * Returns 0 for a clean close (the client sent FIN and readline() returned
 * 0), 1 if the connection died on an error.
 */
static int echo_service(int connfd, const char *peer)
{
    char    buf[ECHO_BUFSIZE];
    ssize_t n;

    readline_reset();

    for (;;) {
        n = readline(connfd, buf, sizeof(buf));

        if (n < 0) {
            log_err("%s: readline failed", peer);
            return 1;
        }

        if (n == 0) {
            /* read() returned 0: the client closed (Ctrl-D at its stdin,
             * per step 6 of the handout).  Normal termination. */
            log_info("%s: EOF from client", peer);
            return 0;
        }

        if (writen(connfd, buf, (size_t)n) != n) {
            log_err("%s: writen failed after %zd bytes", peer, n);
            return 1;
        }

        log_info("%s: echoed %zd byte%s%s", peer, n, (n == 1) ? "" : "s",
                 (buf[n - 1] == '\n') ? "" : " (no newline: line limit reached)");
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    int                listenfd;
    int                reuse = 1;
    uint16_t           port;
    struct sockaddr_in servaddr;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    port = parse_port(argv[1]);

    install_parent_handlers();

    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        die_sys("socket");

    /* Without SO_REUSEADDR a restart within the 2*MSL TIME_WAIT window
     * fails with EADDRINUSE -- painful during testing. */
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) < 0)
        die_sys("setsockopt(SO_REUSEADDR)");

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family      = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);   /* all interfaces */
    servaddr.sin_port        = htons(port);

    if (bind(listenfd, (const struct sockaddr *)&servaddr,
             sizeof(servaddr)) < 0)
        die_sys("bind to port %u", (unsigned)port);

    if (listen(listenfd, ECHO_LISTENQ) < 0)
        die_sys("listen");

    log_info("echos: listening on 0.0.0.0:%u (pid %ld)",
             (unsigned)port, (long)getpid());

    while (!shutdown_requested) {
        struct sockaddr_in cliaddr;
        socklen_t          clilen = sizeof(cliaddr);
        char               peer[PEERSTR_MAX];
        int                connfd;
        pid_t              pid;

        connfd = accept(listenfd, (struct sockaddr *)&cliaddr, &clilen);

        if (connfd < 0) {
            /* EINTR: a signal arrived while we were blocked.  Either we are
             * shutting down, or it was SIGCHLD -- loop round either way. */
            if (errno == EINTR)
                continue;

            /* The client sent RST between the handshake and our accept().
             * Nothing is wrong with the server; drop it and keep going. */
            if (errno == ECONNABORTED || errno == EPROTO)
                continue;

            /* Transient resource exhaustion: do not kill the server. */
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS ||
                errno == ENOMEM) {
                log_err("accept: out of resources, continuing");
                sleep(1);
                continue;
            }

            die_sys("accept");
        }

        format_peer(&cliaddr, peer, sizeof(peer));

        if ((pid = fork()) < 0) {
            log_err("fork failed, dropping connection from %s", peer);
            close(connfd);
            continue;
        }

        if (pid == 0) {
            /* ---- child ---------------------------------------------- */
            int status;

            /* The listening socket was open before fork(), so the child has
             * a copy.  Close it: otherwise the port stays bound after the
             * parent exits, and the child holds a descriptor it never uses. */
            if (close(listenfd) < 0)
                log_err("%s: child close(listenfd)", peer);

            reset_child_handlers();

            log_info("%s: connection established (child pid %ld)",
                     peer, (long)getpid());

            status = echo_service(connfd, peer);

            if (close(connfd) < 0)
                log_err("%s: close(connfd)", peer);

            log_info("%s: connection closed (child pid %ld exiting)",
                     peer, (long)getpid());

            /* _exit(), not exit(): skip the parent's inherited stdio
             * cleanup so nothing gets flushed twice. */
            _exit(status);
        }

        /* ---- parent -------------------------------------------------- */
        /* The child owns the connected socket now.  The parent must close
         * its copy or the descriptor leaks and the peer never sees FIN when
         * the child eventually exits. */
        if (close(connfd) < 0)
            log_err("%s: parent close(connfd)", peer);

        log_info("%s: accepted, handed to child pid %ld", peer, (long)pid);
    }

    log_info("echos: shutdown requested, closing listening socket");
    close(listenfd);

    /* Give already-running children a moment to finish, then reap. */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;

    return EXIT_SUCCESS;
}
