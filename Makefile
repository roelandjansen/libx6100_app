# SPDX-License-Identifier: GPL-3.0-or-later
#
# libx6100_app top-level Makefile
#
# make TARGET=native|x6100|android-arm64 [DEBUG=1]
# make TARGET=native test
# make TARGET=android-arm64 apk
# make clean TARGET=...
#

-include config.mk

TARGET ?= native
DEBUG  ?= 0

C_TARGETS := native x6100 android-arm64

ifeq ($(filter $(TARGET),$(C_TARGETS)),)
$(error unknown TARGET '$(TARGET)', use one of: $(C_TARGETS))
endif

ifeq ($(DEBUG),1)
BUILD := build/$(TARGET)-debug
else
BUILD := build/$(TARGET)
endif

.DEFAULT_GOAL := all

include mk/common.mk
include mk/target-$(TARGET).mk

.PHONY: all clean test apk

# keep object files of test programs (otherwise removed as intermediates)
.SECONDARY:

all: $(OUT)

clean:
	rm -rf -- $(BUILD)

-include $(DEPS)
