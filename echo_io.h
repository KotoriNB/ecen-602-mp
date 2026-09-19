/*
 * echo_io.h -- Shared socket I/O helpers for the ECEN 602 MP1 echo service.
 *
 * These two functions are the ones the assignment asks us to write
 * ourselves (Figure 2 of the handout): writen() and readline().  They are
 * shared by the server (echos) and the client (echo) so that both halves of
 * the team agree on framing and on the maximum line length.
 *
 * ECEN 602 -- Machine Problem 1
 */

#ifndef ECHO_IO_H
#define ECHO_IO_H

#include <stddef.h>
#include <sys/types.h>

/*
 * ECHO_MAXLINE is the maximum number of payload bytes we will hand back to
 * the caller in a single readline() call.  ECHO_BUFSIZE is "max line + 1":
 * the extra byte holds the '\0' terminator, exactly as the handout
 * describes.  Every buffer passed to readline() must be ECHO_BUFSIZE bytes.
 *
 * NOTE: this is *not* a protocol limit.  A peer that sends a longer line
 * simply gets it echoed back in ECHO_MAXLINE-sized pieces; no bytes are
 * lost and the connection is not torn down.  See README.md ("Long lines").
 */
#define ECHO_MAXLINE 4096
#define ECHO_BUFSIZE (ECHO_MAXLINE + 1)

/*
 * writen -- write exactly n bytes to a descriptor.
 *
 * A write() on a TCP socket may transfer fewer bytes than requested when the
 * kernel socket buffer fills up; that is a short count, not an error.  This
 * loops until all n bytes are out.  EINTR from a signal restarts the write
 * rather than surfacing a short count to the caller.
 *
 * Returns n on success, or -1 with errno set on a real error (e.g. EPIPE
 * when the peer has closed its end).
 */
ssize_t writen(int fd, const void *vptr, size_t n);

/*
 * readline -- read one '\n'-terminated line from a descriptor.
 *
 *   fd      socket (or file) descriptor to read from
 *   vptr    caller's buffer
 *   maxlen  size of that buffer, i.e. max line length + 1
 *
 * Reads until one of:
 *   (1) a '\n' is read              -- the '\n' IS stored, like fgets()
 *   (2) maxlen-1 bytes are stored   -- long line, returned without a '\n'
 *   (3) EOF (read() returns 0)      -- returns whatever was read so far
 * The buffer is always '\0'-terminated.  EINTR is handled internally by
 * re-issuing the read.
 *
 * Returns the number of bytes stored, NOT counting the '\0'.  A return of 0
 * means EOF with nothing buffered (the peer closed the connection).  Returns
 * -1 with errno set on error.
 *
 * Implementation note: this is the efficient version from the handout -- it
 * read()s into a private static buffer and doles out one character at a
 * time, so a 4096-byte line costs ~1 system call instead of 4096.  The
 * static state makes it neither thread-safe nor re-entrant, which is fine
 * here because the server gives every connection its own *process*.
 */
ssize_t readline(int fd, void *vptr, size_t maxlen);

/*
 * readline_reset -- discard any bytes readline() has buffered internally.
 *
 * Call this before using readline() on a new descriptor so that leftover
 * bytes from a previous connection can never leak into the new one.  (In
 * the fork() server each child gets a fresh copy of the static state, so
 * this is belt-and-braces, but it keeps the function honest if the code is
 * ever reused in a single-process server.)
 */
void readline_reset(void);

#endif /* ECHO_IO_H */
