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
 * which case the logging calls below are harmless no-ops.
 *
 * With `old_path`, the log never grows without bound: log_fresh() and a log
 * past LOG_LIMIT both move it to `old_path` (replacing what was there) and
 * start it empty, so the current and the previous log are always kept. */
int  log_open(const char *dir, const char *path, const char *old_path);
void log_fresh(void);
void log_close(void);

/* Safe to call from any thread. */
void log_line(const char *fmt, ...);

/* Puts a message in the console's notification area. */
void notify(const char *fmt, ...);

#endif
