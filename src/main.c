/* FGG-XSense -- use an Xbox-protocol controller on a PlayStation 5.
 *
 * A PS5 enumerates a GIP gamepad and then ignores it entirely: no light, no
 * input, not even a complaint. This payload picks it up and republishes it to
 * the system as an ordinary PlayStation pad. Nothing in the system software is
 * patched, nothing is installed, and the virtual pad is removed on exit.
 *
 *   gip.c   reads the controller over USB
 *   vpad.c  publishes a virtual pad to the kernel and translates reports
 *   main.c  the lock, the event loop, and keeping a stalling controller alive
 *
 * The PS button is not mapped, and cannot be -- see the note in gip.h and the
 * README. Everything else works.
 */
#include "gip.h"
#include "log.h"
#include "vpad.h"

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define STATE_DIR "/data/fgg-xsense"
#define LOG_PATH  STATE_DIR "/xsense.log"
#define LOCK_PATH STATE_DIR "/xsense.lock"

/* How stale the lock must be before another copy claims it. The running
 * payload refreshes it every second, so anything older belongs to a copy that
 * died without cleaning up. */
#define LOCK_STALE_SECONDS 15

/* Silence, in seconds, before concluding the controller is really gone.
 *
 * This has to be measured in time rather than in failed reads. Failed reads
 * are entirely normal here, and a failure returns immediately with no wait, so
 * counting them declares an unplug within microseconds of starting.
 */
#define UNPLUG_SECONDS 30

/* Silence that triggers re-initialisation instead of giving up. */
#define REINIT_SECONDS 3

/* Re-initialise this often even while everything is healthy. Waiting for the
 * controller to fall silent first costs a few seconds of input every time. */
#define KEEPALIVE_SECONDS 20

/* Consecutive fruitless re-initialisations before rebuilding the USB
 * connection, which is the heavier remedy for a wedged endpoint. */
#define REINITS_BEFORE_REBUILD 3

/* Hold View+Menu together this long to stop the payload deliberately. Held
 * rather than tapped, so it cannot fire by accident mid-game. */
#define QUIT_HOLD_SECONDS 2

/* How often to summarise progress into the log. */
#define REPORT_SECONDS 60

/* The user the virtual pad belongs to. */
#define VPAD_USER 1

/* Refuses to start when another copy is already running.
 *
 * Launching twice is easy to do by accident, and two copies is not merely
 * wasteful: both claim the same USB interface, both queue reads on the same
 * endpoint, and each consumes completions belonging to the other. It presents
 * exactly as failing hardware -- a controller that constantly goes quiet and
 * gets reinitialised -- which took a long time to recognise for what it was.
 */
static int lock_take(void)
{
    struct stat stv;
    int fd;

    if (stat(LOCK_PATH, &stv) == 0 &&
        time(NULL) - stv.st_mtime < LOCK_STALE_SECONDS)
        return 0;

    fd = open(LOCK_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static void lock_refresh(void) { utimes(LOCK_PATH, NULL); }
static void lock_release(void) { unlink(LOCK_PATH); }

/* Everything the event loop counts, kept together so the periodic line and the
 * summary at the end cannot drift apart. */
typedef struct {
    long reports, delivered, failed, acked;
    int  reinits, rebuilds;
} stats;

/* Returns when the controller is unplugged, the quit combination is held, or
 * the connection cannot be recovered. */
static void run(gip_device *dev, vpad *pad, stats *st)
{
    unsigned char report[VPAD_REPORT_SIZE];
    time_t last_input, last_reinit, last_keepalive, last_note;
    time_t last_beat = 0, quit_since = 0;
    int since_input = 0;

    last_input = last_reinit = last_keepalive = last_note = time(NULL);

    for (;;) {
        const unsigned char *msg = NULL;
        time_t now;
        int n = gip_read(dev, &msg);

        if (n < 0) {
            /* Back off briefly so a refused transfer does not spin the CPU. */
            st->failed++;
            usleep(2000);
        } else if (n == 0) {
            usleep(1000);
        } else if (n >= 4) {
            last_input = time(NULL);
            since_input = 0;

            /* Answer anything that asked to be answered before interpreting
             * it: the controller waits, and eventually stops talking to a host
             * that ignores it. */
            if (msg[1] & GIP_OPT_ACK) {
                gip_acknowledge(dev, msg);
                st->acked++;
            }

            if (msg[0] == GIP_LL_INPUT_REPORT && n >= 4 + 14) {
                unsigned buttons = (unsigned)msg[4] | ((unsigned)msg[5] << 8);

                st->reports++;

                if ((buttons & (GIP_VIEW | GIP_MENU)) == (GIP_VIEW | GIP_MENU)) {
                    if (quit_since == 0) {
                        quit_since = time(NULL);
                    } else if (time(NULL) - quit_since >= QUIT_HOLD_SECONDS) {
                        log_line("  View+Menu held -- stopping");
                        return;
                    }
                } else {
                    quit_since = 0;
                }

                vpad_translate(msg, report);
                if (vpad_submit(pad, report)) st->delivered++;
            }
        }

        /* Housekeeping runs on every pass, including ones where a message
         * arrived. An earlier version skipped it whenever the guide button was
         * pressed, which stalled the heartbeat and every timer below it. */
        now = time(NULL);

        if (now != last_beat) {
            last_beat = now;
            lock_refresh();
        }

        if (now - last_keepalive >= KEEPALIVE_SECONDS) {
            last_keepalive = now;
            gip_init(dev);
        }

        if (now - last_input >= REINIT_SECONDS &&
            now - last_reinit >= REINIT_SECONDS) {
            last_reinit = last_keepalive = now;
            st->reinits++;
            gip_init(dev);

            if (++since_input >= REINITS_BEFORE_REBUILD) {
                since_input = 0;
                st->rebuilds++;
                log_line("  re-initialising is not helping -- rebuilding USB");
                if (!gip_rebuild(dev)) {
                    log_line("  rebuild failed -- giving up");
                    return;
                }
            }
        }

        if (now - last_input >= UNPLUG_SECONDS) {
            log_line("  no input for %d seconds -- controller gone",
                     UNPLUG_SECONDS);
            return;
        }

        if (now - last_note >= REPORT_SECONDS) {
            last_note = now;
            log_line("  %ld read, %ld delivered, %ld acked, %d reinit(s), "
                     "%d rebuild(s)",
                     st->reports, st->delivered, st->acked,
                     st->reinits, st->rebuilds);
        }
    }
}

int main(void)
{
    char why[160];
    gip_device *dev = NULL;
    vpad *pad = NULL;
    stats st;
    int rc = 1;

    memset(&st, 0, sizeof st);

    if (!log_open(STATE_DIR, LOG_PATH)) return 1;

    log_line("========================================");
    log_line("FGG-XSense -- Xbox controller on PlayStation 5");

    if (!lock_take()) {
        log_line("  another copy is already running -- exiting");
        notify("FGG-XSense: already running");
        log_close();
        return 1;
    }

    dev = gip_open(why, (int)sizeof why);
    if (!dev) {
        log_line("  %s", why);
        notify("FGG-XSense: %s", why);
        goto done;
    }
    log_line("  controller %s  %04X:%04X  %s",
             gip_node(dev), gip_vendor(dev), gip_product(dev), why);

    pad = vpad_open(VPAD_USER, why, (int)sizeof why);
    if (!pad) {
        log_line("  could not register a virtual pad -- %s", why);
        notify("FGG-XSense: could not register a virtual pad");
        goto done;
    }
    log_line("  virtual pad registered, %s", why);

    notify("FGG-XSense: controller ready - hold View+Menu to stop");
    log_line("  running -- unplug the controller or hold View+Menu to stop");

    run(dev, pad, &st);

    log_line("  total: %ld read, %ld delivered, %ld failed, %ld acked, "
             "%d reinit(s), %d rebuild(s)",
             st.reports, st.delivered, st.failed, st.acked,
             st.reinits, st.rebuilds);

    if (st.delivered > 0)
        notify("FGG-XSense: stopped - %ld inputs delivered", st.delivered);
    else
        notify("FGG-XSense: no input delivered - see the log");

    rc = 0;

done:
    /* One exit path, so a pad cannot be left registered and the lock cannot be
     * left behind by whichever failure happened to occur. */
    vpad_close(pad);
    gip_close(dev);
    lock_release();
    log_close();
    return rc;
}
