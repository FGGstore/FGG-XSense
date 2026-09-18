/* Logging to a file that survives the payload, and system notifications.
 *
 * A payload has no console and no debugger worth the name, so the log is the
 * only account of what happened. It is opened in append mode and flushed after
 * every line: a run that ends in a crash still leaves everything up to the
 * crash on disk, which is what made the difference in diagnosing this one.
 */
#ifndef FGG_LOG_H
#define FGG_LOG_H

/* Opens the log at `path`, creating `dir` if needed. Returns 0 on failure, in
 * which case the logging calls below are harmless no-ops. */
int  log_open(const char *dir, const char *path);
void log_close(void);

void log_line(const char *fmt, ...);

/* Puts a message in the console's notification area. */
void notify(const char *fmt, ...);

#endif
