PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := zsshd.elf

# Prebuilt libssh + mbedtls (installed into the CMake prefix)
PREFIX := external/prefix

SRCS := \
	src/main.c \
	src/app/zsshd_instance.c \
	src/app/instance_control.c \
	src/app/ssh_server.c \
	src/app/shell.c \
	src/platform/pal_notification.c

CFLAGS := -Wall -Werror -g
CFLAGS += -DPLATFORM_PS5
CFLAGS += -Iinclude
CFLAGS += -Iinclude/platform
CFLAGS += -I$(PREFIX)/include

LDFLAGS := -L$(PREFIX)/lib
# Link order matters: libssh depends on the mbedtls trio, which in turn
# pulls in the Everest/p256-m ECC backends.
LDLIBS := -lssh -lmbedtls -lmbedx509 -lmbedcrypto -leverest -lp256m

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(ELF)

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^
