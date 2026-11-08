# SPDX-License-Identifier: GPL-3.0-or-later
#
# Target: x6100 (ARMv7 hard-float, glibc pinned to the radio's version)
#

ifeq ($(X6100_GLIBC),)
$(error X6100_GLIBC not set, see config.mk.example)
endif

CC := zig cc -target arm-linux-gnueabihf.$(X6100_GLIBC)

# Extra libraries from the radio (ALSA etc.). zig provides glibc itself.
ifneq ($(X6100_SYSROOT),)
C_SYSROOT_CFLAGS := -isystem $(X6100_SYSROOT)/usr/include
C_SYSROOT_LIBS   := -L$(X6100_SYSROOT)/usr/lib -L$(X6100_SYSROOT)/lib
else
C_SYSROOT_CFLAGS :=
C_SYSROOT_LIBS   :=
endif

CFLAGS_TARGET := -mcpu=cortex-a7 $(C_SYSROOT_CFLAGS)
LDFLAGS_TARGET := $(C_SYSROOT_LIBS)
LDLIBS_TARGET := -lm -lpthread

SRC_PLATFORM := $(wildcard platform/x6100/*.c)
OBJS := $(call objs,$(SRC_COMMON) $(SRC_PLATFORM))
DEPS := $(OBJS:.o=.d)

OUT := $(BUILD)/radio

# Link, then refuse any glibc symbol version above the pinned one
$(OUT): $(OBJS)
	$(CC) $(LDFLAGS_TARGET) -o $@ $^ $(LDLIBS_TARGET)
	@L_MAX=$$(readelf --dyn-syms -W -- $@ | grep -o 'GLIBC_[0-9.]*' | cut -d_ -f2 | sort -V -u | tail -n 1); \
	if [ -n "$$L_MAX" ] && [ "$$(printf '%s\n%s\n' "$$L_MAX" "$(X6100_GLIBC)" | sort -V | tail -n 1)" != "$(X6100_GLIBC)" ]; then \
		printf 'error: %s needs GLIBC_%s, radio has %s\n' "$@" "$$L_MAX" "$(X6100_GLIBC)" >&2; \
		rm -f -- $@; exit 1; \
	fi

test:
	$(error tests run on the host, use TARGET=native test)

apk:
	$(error apk is only available for TARGET=android-arm64)
