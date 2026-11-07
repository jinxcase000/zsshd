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
	src/app/zsshd_config.c \
	src/app/firstrun.c \
	src/app/shell.c \
	src/platform/pal_notification.c \
	src/platform/ps5_syscall.S

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

# The SDK libc's raw system call stubs (src/platform/ps5_raw_syscalls.txt)
# report a failure as a positive return value with errno untouched. The ones
# the build calls are wrapped: a call goes to the error-reporting wrapper in
# src/platform/ps5_syscall.S (keep RAW_WRAPPED and that file in step). The
# linker traces every other stub name, and the build fails if anything
# references one, so a new call to a raw stub cannot slip in.
RAW_WRAPPED := link symlink readlink chown fchown utimensat getfsstat statfs getpgrp \
	syscall __syscall
RAW_STUBS := $(shell cat src/platform/ps5_raw_syscalls.txt)
LDFLAGS := $(foreach s,$(RAW_WRAPPED),-Wl,--wrap=$(s))
LDFLAGS += $(foreach s,$(filter-out $(RAW_WRAPPED),$(RAW_STUBS)),-Wl,--trace-symbol=$(s))

all: $(ELF)

$(ELF): $(SRCS) src/platform/ps5_raw_syscalls.txt
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LIBS) > $@.trace
	@if grep ': reference to ' $@.trace; then \
	  echo "error: the lines above call raw SDK syscall stubs that report failure as success; wrap them in src/platform/ps5_syscall.S and RAW_WRAPPED" >&2; \
	  rm -f $@; exit 1; \
	fi; rm -f $@.trace

clean:
	rm -f $(ELF) $(ELF).trace

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^
