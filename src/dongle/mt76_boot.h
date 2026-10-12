/* Getting firmware running on the adapter.
 *
 * Cold (nothing loaded since the adapter was powered): the image is uploaded
 * and started. Warm (firmware already running, e.g. the payload restarted):
 * it is reset first, then either uploaded again or left running, depending
 * on what the reset leaves (see warm() in mt76_boot.c).
 * Either way the chip may briefly drop off the bus as the firmware starts;
 * that is reported, not treated as a failure.
 */
#ifndef FGG_MT76_BOOT_H
#define FGG_MT76_BOOT_H

#include "dongle/mt76.h"
#include "dongle/mt76_fw.h"

typedef enum {
    MT76_BOOT_COLD,      /* uploaded and started */
    MT76_BOOT_WARM,      /* was already running; reset and re-uploaded */
    MT76_BOOT_RESTARTED, /* was already running; restarted in place */
    MT76_BOOT_GONE,      /* left the bus while starting: reopen and retry */
    MT76_BOOT_FAILED,
} mt76_boot_result;

mt76_boot_result mt76_boot(mt76 *m, const mt76_fw *fw);

const char *mt76_boot_name(mt76_boot_result r);

#endif
