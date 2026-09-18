/* Publishing a virtual controller to the PlayStation 5.
 *
 * libScePad's virtual-device functions turn out to be thin wrappers over three
 * ioctls on /dev/hid, and not one of them checks who is calling -- only that
 * the device is open. So an ordinary payload can register a controller and
 * feed it input with no patching of SceShellCore or anything else. All eleven
 * device types the kernel recognises were accepted on firmware 11.60.
 *
 * The library itself is not used: it is not loaded in the process a payload is
 * injected into, and calling into it faults.
 */
#ifndef FGG_VPAD_H
#define FGG_VPAD_H

#include <stdint.h>

typedef struct vpad vpad;

/* Registers a virtual controller owned by `user`. Returns NULL on failure with
 * a short reason in `why`. */
vpad *vpad_open(uint32_t user, char *why, int whylen);

/* Removes the device. A pad left registered outlives the payload. */
void vpad_close(vpad *pad);

int vpad_handle(const vpad *pad);

/* Publishes one frame of input, already translated. Returns 0 on failure. */
int vpad_submit(vpad *pad, const unsigned char *report);

/* Translates a GIP input report into the report the kernel reads.
 *
 * `report` must be VPAD_REPORT_SIZE bytes. Note that this is emphatically not
 * a ScePadData: libScePad translates its caller's ScePadData into a
 * differently shaped buffer before handing it over, and passing a ScePadData
 * straight through appears to work -- the ioctl validates only its 24-byte
 * header, never the contents -- while the kernel reads the buttons out of what
 * was actually the orientation quaternion. The result is a controller the
 * system acknowledges and that does nothing at all.
 */
#define VPAD_REPORT_SIZE 0xA0

void vpad_translate(const unsigned char *gip_report, unsigned char *report);

#endif
