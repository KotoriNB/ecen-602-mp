/*
 * echo_io.c -- Implementation of writen() and readline().
 *
 * ECEN 602 -- Machine Problem 1
 */

#include "echo_io.h"

#include <errno.h>
#include <unistd.h>

/* Size of readline()'s private staging buffer.  Independent of
 * ECHO_MAXLINE: it only controls how many bytes we pull from the kernel per
 * system call. */
#define READ_CHUNK 4096

static int   read_cnt;              /* bytes left unread in read_buf     */
static char *read_ptr;              /* next unread byte in read_buf      */
static char  read_buf[READ_CHUNK];  /* staging buffer for buffered_read  */

/*
 * buffered_read -- hand back exactly one character, refilling from the
 * kernel only when the private buffer runs dry.
 *
 * Returns 1 on success (*ptr set), 0 on EOF, -1 on error (errno set).
 * EINTR is retried here so that callers never see a spurious failure.
 */
static ssize_t buffered_read(int fd, char *ptr)
{
    while (read_cnt <= 0) {
        ssize_t nread = read(fd, read_buf, sizeof(read_buf));

        if (nread < 0) {
            if (errno == EINTR)
                continue;           /* slow system call interrupted: retry */
            return -1;
        }
        if (nread == 0)
            return 0;               /* EOF */

        read_cnt = (int)nread;
        read_ptr = read_buf;
    }

    read_cnt--;
    *ptr = *read_ptr++;
    return 1;
}

void readline_reset(void)
{
    read_cnt = 0;
    read_ptr = read_buf;
}

ssize_t readline(int fd, void *vptr, size_t maxlen)
{
    char  *ptr = vptr;
    size_t n   = 0;

    /* maxlen is "max line + 1"; we need at least room for the '\0'. */
    if (ptr == NULL || maxlen == 0) {
        errno = EINVAL;
        return -1;
    }

    while (n + 1 < maxlen) {
        char    c;
        ssize_t rc = buffered_read(fd, &c);

        if (rc == 1) {
            ptr[n++] = c;
            if (c == '\n')
                break;              /* newline stored, like fgets() */
        } else if (rc == 0) {
            break;                  /* EOF: return what we have (maybe 0) */
        } else {
            return -1;              /* real error, errno set by read() */
        }
    }

    ptr[n] = '\0';
    return (ssize_t)n;
}

ssize_t writen(int fd, const void *vptr, size_t n)
{
    const char *ptr   = vptr;
    size_t      nleft = n;

    if (ptr == NULL && n != 0) {
        errno = EINVAL;
        return -1;
    }

    while (nleft > 0) {
        ssize_t nwritten = write(fd, ptr, nleft);

        if (nwritten <= 0) {
            if (nwritten < 0 && errno == EINTR)
                continue;           /* interrupted before any byte moved */
            return -1;              /* EPIPE, ECONNRESET, ... */
        }

        nleft -= (size_t)nwritten;
        ptr   += nwritten;
    }

    return (ssize_t)n;
}
