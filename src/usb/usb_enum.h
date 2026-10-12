/* Finding devices, and keeping backends off each other's.
 *
 * Every backend finds its device by scanning the ugen nodes, and a scan opens
 * each node to read its descriptors. A node already driven by a backend must
 * not be opened by anyone else -- a second opener on a device in use is
 * exactly the confusion described in main.c's lock comment -- so backends own
 * their nodes here and every scan skips owned ones.
 */
#ifndef FGG_USB_ENUM_H
#define FGG_USB_ENUM_H

#define USB_NODE_LEN 32

/* Decides from a device's identity and configuration descriptor whether a
 * backend wants it. */
typedef int (*usb_match_fn)(unsigned vendor, unsigned product,
                            const unsigned char *cfg, int cfglen);

/* Finds the first unowned node `match` accepts and takes ownership of it.
 * Returns 1 and the node's path in `node`, or 0 if there is none. */
int usb_find_and_own(usb_match_fn match, char *node);

/* Gives a node back once its backend is finished with it. */
void usb_disown(const char *node);

#endif
