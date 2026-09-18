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

CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 -Isrc

# No libScePad: the host process has not loaded it, so calls into the module
# fault. The ioctls it wraps are issued directly instead.

SRCS := src/main.c src/gip.c src/vpad.c src/log.c
OBJS := $(patsubst %.c,$(BUILD)/%.o,$(SRCS))

.PHONY: all clean test

all: $(ELF)

$(ELF): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

clean:
	rm -rf $(BUILD) $(ELF)
