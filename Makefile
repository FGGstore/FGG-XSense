# FGG-XSense
#
#   export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
#   make

PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF   := fgg-xsense.elf
BUILD := build

CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 -Isrc \
          -Ithird_party/xone -Ithird_party/compat

# No libScePad: the host process has not loaded it, so calls into the module
# fault. The ioctls it wraps are issued directly instead.

SRCS := src/main.c src/pads.c src/vpad.c src/lock.c src/config.c src/log.c \
        src/gip/gip_proto.c src/gip/gip_wired.c \
        src/usb/ugen.c src/usb/usb_enum.c src/sha256.c \
        src/dongle/dongle.c src/dongle/mt76_fw.c \
        src/dongle/mt76.c src/dongle/mt76_boot.c src/dongle/mt76_radio.c \
        src/dongle/link.c
OBJS := $(patsubst %.c,$(BUILD)/%.o,$(SRCS))

.PHONY: all clean test probe check

all: $(ELF)

$(ELF): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# -MMD records each object's headers, so changing a header rebuilds every
# object that includes it -- without it, a stale object can disagree with the
# others about an enum or a struct.
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(OBJS:.o=.d) $(BUILD)/tools/probe.d

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

# Read-only hardware survey for the wireless backends: see tools/probe.c.
probe: probe.elf

probe.elf: $(BUILD)/tools/probe.o $(BUILD)/src/log.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Host-side tests of everything that touches no device, built with the host's
# own compiler. Named check because `test` deploys to the console.
HOSTCC ?= gcc
HOST_TESTS := tests/test_core.c src/gip/gip_proto.c src/vpad.c src/sha256.c \
              src/dongle/mt76_fw.c

check:
	@mkdir -p $(BUILD)/host
	$(HOSTCC) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Isrc -Itests \
		-Ithird_party/xone -Ithird_party/compat \
		-o $(BUILD)/host/test_core $(HOST_TESTS)
	$(BUILD)/host/test_core

clean:
	rm -rf $(BUILD) $(ELF) probe.elf
