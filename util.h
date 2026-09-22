// util.h -- timestamped logging and fatal-error reporting.
// Each message is written with a single write() so concurrent children do not
// interleave mid-line.

#ifndef UTIL_H
#define UTIL_H

void log_info(const char *fmt, ...);            // -> stdout
void log_err(const char *fmt, ...);             // -> stderr, appends strerror
void log_msg(const char *fmt, ...);             // -> stderr, no errno
void die_sys(const char *fmt, ...);             // log_err, then exit(1)
void die_msg(const char *fmt, ...);             // log_msg, then exit(1)

#endif // UTIL_H
