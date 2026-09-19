/*
 * util.h -- Timestamped logging and fatal-error helpers.
 *
 * The handout asks us to print a readable description for every failed
 * network call (the err_sys() idea from UNP).  These helpers do that and
 * add a timestamp, which makes the screen captures for the test report much
 * easier to read when several client processes are active at once.
 *
 * Each message is formatted into a stack buffer and emitted with a single
 * write() so that lines from the parent and from several children do not
 * interleave mid-line on a shared terminal.
 *
 * ECEN 602 -- Machine Problem 1
 */

#ifndef UTIL_H
#define UTIL_H

/* Informational message -> stdout. */
void log_info(const char *fmt, ...);

/* Error message + ": " + strerror(errno) -> stderr.  errno is preserved. */
void log_err(const char *fmt, ...);

/* Error message with no errno suffix -> stderr. */
void log_msg(const char *fmt, ...);

/* log_err() then exit(EXIT_FAILURE).  For unrecoverable system-call errors. */
void die_sys(const char *fmt, ...);

/* log_msg() then exit(EXIT_FAILURE).  For usage / argument errors. */
void die_msg(const char *fmt, ...);

#endif /* UTIL_H */
