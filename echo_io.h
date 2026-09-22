// echo_io.h -- writen() and readline(), shared by the server and the client.

#ifndef ECHO_IO_H
#define ECHO_IO_H

#include <stddef.h>
#include <sys/types.h>

// Buffer size, not a protocol limit: a longer line is echoed back in
// ECHO_MAXLINE-sized pieces, never truncated. The +1 holds the '\0'.
#define ECHO_MAXLINE 4096
#define ECHO_BUFSIZE (ECHO_MAXLINE + 1)

// Writes exactly n bytes, looping over short counts. Returns n, or -1.
ssize_t writen(int fd, const void *vptr, size_t n);

// Reads until '\n' (stored, like fgets()), maxlen-1 bytes, or EOF, and always
// '\0'-terminates. Returns bytes stored; 0 means EOF with nothing buffered.
// Not reentrant: the internal buffer is static, which is fine because each
// connection gets its own process.
ssize_t readline(int fd, void *vptr, size_t maxlen);

// Drops readline()'s buffered bytes so they cannot leak into a new connection.
void readline_reset(void);

#endif // ECHO_IO_H
