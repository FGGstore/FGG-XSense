#include "usb/usb_enum.h"
#include "usb/ugen.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_OWNED 16
#define CFG_MAX   4096

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_owned[MAX_OWNED][USB_NODE_LEN];

static int owned(const char *node)
{
    int i;
    for (i = 0; i < MAX_OWNED; i++)
        if (strcmp(g_owned[i], node) == 0) return 1;
    return 0;
}

static int own(const char *node)
{
    int i;
    for (i = 0; i < MAX_OWNED; i++) {
        if (g_owned[i][0] == '\0') {
            snprintf(g_owned[i], USB_NODE_LEN, "%s", node);
            return 1;
        }
    }
    return 0;
}

int usb_find_and_own(usb_match_fn match, char *node)
{
    static unsigned char cfg[CFG_MAX];
    int bus, unit, found = 0;

    /* Held across the whole scan: cfg is shared, and two scans racing could
     * otherwise hand one node to two backends. */
    pthread_mutex_lock(&g_lock);

    for (bus = 0; bus < 8 && !found; bus++) {
        for (unit = 1; unit < 16 && !found; unit++) {
            unsigned vendor, product;
            int len;
            ugen *u;

            snprintf(node, USB_NODE_LEN, "/dev/ugen%d.%d", bus, unit);
            if (owned(node) || access(node, F_OK) != 0) continue;

            u = ugen_open(node);
            if (!u) continue;

            /* The descriptor's own length, not the buffer's: walking past the
             * end reads stale data and can invent endpoints. */
            len = ugen_config_desc(u, cfg, (int)sizeof cfg);
            if (ugen_ids(u, &vendor, &product) && len > 0 &&
                match(vendor, product, cfg, len))
                found = own(node);

            ugen_close(u);
        }
    }

    pthread_mutex_unlock(&g_lock);
    return found;
}

void usb_disown(const char *node)
{
    int i;

    pthread_mutex_lock(&g_lock);
    for (i = 0; i < MAX_OWNED; i++)
        if (strcmp(g_owned[i], node) == 0) g_owned[i][0] = '\0';
    pthread_mutex_unlock(&g_lock);
}
