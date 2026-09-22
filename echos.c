// echos.c -- TCP server
// Usage:  ./echos <port>

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

#define ECHO_LISTENQ 128
#define PEERSTR_MAX 32

static volatile sig_atomic_t shutdown_requested = 0;

// Signals do not queue, so one SIGCHLD can cover several exits: loop.
// errno is saved for the main-line call this interrupted.
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
    // SA_RESTART so a child exiting does not abort a blocked accept().
    install_handler(SIGCHLD, sigchld_handler, SA_RESTART | SA_NOCLDSTOP);

    // No SA_RESTART: accept() must return EINTR to notice shutdown_requested.
    install_handler(SIGINT,  shutdown_handler, 0);
    install_handler(SIGTERM, shutdown_handler, 0);

    // Ignored so writen() reports EPIPE instead of the process dying.
    install_handler(SIGPIPE, SIG_IGN, 0);
}

// The child has no children to reap and should just die on Ctrl-C.
static void reset_child_handlers(void)
{
    install_handler(SIGCHLD, SIG_DFL, 0);
    install_handler(SIGINT,  SIG_DFL, 0);
    install_handler(SIGTERM, SIG_DFL, 0);
}

// strtol(), not atoi(), so "80x" and out-of-range values are rejected.
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

static void format_peer(const struct sockaddr_in *addr, char *out, size_t len)
{
    char ip[INET_ADDRSTRLEN];

    if (inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)) == NULL)
        snprintf(ip, sizeof(ip), "?.?.?.?");

    snprintf(out, len, "%s:%u", ip, (unsigned)ntohs(addr->sin_port));
}

// Runs in the child. Returns 0 on a clean close, 1 on error.
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

    // Without this, a restart inside TIME_WAIT fails with EADDRINUSE.
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) < 0)
        die_sys("setsockopt(SO_REUSEADDR)");

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family      = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);   // all interfaces
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

        // None of these are the server's fault; only a real failure is fatal.
        if (connfd < 0) {
            if (errno == EINTR)
                continue;

            if (errno == ECONNABORTED || errno == EPROTO)
                continue;

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
            int status;

            // Or the port stays bound after the parent exits.
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

            // _exit(): skip inherited stdio cleanup so nothing flushes twice.
            _exit(status);
        }

        // Or the descriptor leaks and the peer never sees FIN.
        if (close(connfd) < 0)
            log_err("%s: parent close(connfd)", peer);

        log_info("%s: accepted, handed to child pid %ld", peer, (long)pid);
    }

    log_info("echos: shutdown requested, closing listening socket");
    close(listenfd);

    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;

    return EXIT_SUCCESS;
}
