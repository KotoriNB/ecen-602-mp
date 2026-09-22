// echo_io.c -- implementation of writen() and readline().

#include "echo_io.h"

#include <errno.h>
#include <unistd.h>

#define READ_CHUNK 4096

static int   read_cnt;              // bytes left unread in read_buf
static char *read_ptr;              // next unread byte
static char  read_buf[READ_CHUNK];

// One character per call, refilling only when the buffer runs dry, so a line
// costs about one read() instead of one per character.
static ssize_t buffered_read(int fd, char *ptr)
{
    while (read_cnt <= 0) {
        ssize_t nread = read(fd, read_buf, sizeof(read_buf));

        if (nread < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (nread == 0)
            return 0;

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

    if (ptr == NULL || maxlen == 0) {
        errno = EINVAL;
        return -1;
    }

    // n + 1 < maxlen leaves the last byte for the '\0'.
    while (n + 1 < maxlen) {
        char    c;
        ssize_t rc = buffered_read(fd, &c);

        if (rc == 1) {
            ptr[n++] = c;
            if (c == '\n')
                break;
        } else if (rc == 0) {
            break;
        } else {
            return -1;
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

    // A short count on a socket is not an error, so loop.
    while (nleft > 0) {
        ssize_t nwritten = write(fd, ptr, nleft);

        if (nwritten <= 0) {
            if (nwritten < 0 && errno == EINTR)
                continue;
            return -1;
        }

        nleft -= (size_t)nwritten;
        ptr   += nwritten;
    }

    return (ssize_t)n;
}
