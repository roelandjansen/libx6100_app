# SPDX-License-Identifier: GPL-3.0-or-later
#
# Target: android-arm64 (NDK, bionic), output is the native library for the APK
#

ifeq ($(NDK),)
$(error NDK not set, see config.mk.example)
endif

ANDROID_API ?= 29

C_NDK_BIN := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin
CC := $(C_NDK_BIN)/aarch64-linux-android$(ANDROID_API)-clang

ifeq ($(wildcard $(CC)),)
$(error NDK compiler not found: $(CC))
endif

CFLAGS_TARGET := -fPIC
CFLAGS_TARGET_TP := -fPIC
LDFLAGS_TARGET := -shared -Wl,-z,max-page-size=16384 -Wl,--no-undefined
LDLIBS_TARGET := -laaudio -landroid -llog -lm

SRC_PLATFORM := $(wildcard platform/android/jni/*.c)
OBJS := $(call objs,$(SRC_COMMON) $(SRC_UI) $(SRC_PLATFORM))
DEPS := $(OBJS:.o=.d)

# The platform glue uses LVGL and the operating screen
$(OBJDIR)/platform/android/jni/%.o: platform/android/jni/%.c
	@mkdir -p -- $(dir $@)
	$(CC) $(CFLAGS_COMMON) $(C_UI_FLAGS) $(CFLAGS_TARGET) -c -o $@ $<

OUT := $(BUILD)/lib/arm64-v8a/libradio.so

$(OUT): $(OBJS)
	@mkdir -p -- $(dir $@)
	$(CC) $(LDFLAGS_TARGET) -o $@ $^ $(LDLIBS_TARGET)

export ANDROID_SDK ANDROID_PLATFORM ANDROID_BUILD_TOOLS RADIO_KEYSTORE RADIO_KEYSTORE_PASS

apk: $(OUT)
	tools/apk-build.sh $(BUILD)

test:
	$(error tests run on the host, use TARGET=native test)
