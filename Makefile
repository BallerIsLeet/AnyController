# AnyController — PS5 ELF payload
# Needs the PS5 payload SDK: https://github.com/ps5-payload-dev/sdk (v0.45+)
#
#   export PS5_PAYLOAD_SDK=/path/to/installed/sdk
#   make
#   make send PS5_HOST=192.168.1.50

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is not set)
endif

PS5_HOST ?= ps5
PS5_PORT ?= 9021

TARGET  = anycontroller.elf

SRCS = \
    src/main.c \
    src/controller.c \
    src/vpad.c \
    src/controllers/switch_pro.c \
    src/controllers/xbox360.c \
    src/controllers/ds4.c \
    src/transport/usb.c

OBJS = $(SRCS:.c=.o)

CFLAGS += -Wall -Wextra -Werror \
          -std=c11 \
          -Iinclude \
          -O2

LDFLAGS += -lkernel_sys

.PHONY: all clean send

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

send: $(TARGET)
	python3 send_payload.py $(PS5_HOST) $(TARGET) $(PS5_PORT)
