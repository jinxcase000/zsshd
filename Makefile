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

# Link the vendored static archives by EXPLICIT PATH, not via -L/-lssh. The
# ps5-payload-sdk ships its own OpenSSL-built libssh.a on the default library
# search path, and -lssh would pick that up (undefined OpenSSL symbols like
# BN_new / EVP_PKEY_*) instead of our mbedTLS-built one. Order matters: libssh
# depends on the mbedtls trio, which pulls in the Everest/p256-m ECC backends.
LIBS := $(PREFIX)/lib/libssh.a \
	$(PREFIX)/lib/libmbedtls.a \
	$(PREFIX)/lib/libmbedx509.a \
	$(PREFIX)/lib/libmbedcrypto.a \
	$(PREFIX)/lib/libeverest.a \
	$(PREFIX)/lib/libp256m.a

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^
