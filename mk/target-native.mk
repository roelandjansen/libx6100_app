# SPDX-License-Identifier: GPL-3.0-or-later
#
# Target: native (Linux desktop, SDL2), development build
#

CC ?= cc

C_SDL_CFLAGS := $(patsubst -I%,-isystem %,$(shell pkg-config --cflags sdl2 2>/dev/null))
C_SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)

ifeq ($(C_SDL_LIBS),)
$(error SDL2 not found via pkg-config, install SDL2-devel)
endif

ifeq ($(DEBUG),1)
C_SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
else
C_SAN_FLAGS :=
endif

CFLAGS_TARGET := $(C_SDL_CFLAGS) $(C_SAN_FLAGS)
CFLAGS_TARGET_TP :=
LDFLAGS_TARGET := $(C_SAN_FLAGS)
LDLIBS_TARGET := $(C_SDL_LIBS) -lm -lpthread

SRC_PLATFORM := $(wildcard platform/native/*.c)
OBJS := $(call objs,$(SRC_COMMON) $(SRC_PLATFORM))
DEPS := $(OBJS:.o=.d)

OUT := $(BUILD)/radio

$(OUT): $(OBJS)
	$(CC) $(LDFLAGS_TARGET) -o $@ $^ $(LDLIBS_TARGET)

#
# Host unit tests: each tests/*.c is one test program, linked with the common sources
#

SRC_TESTS := $(wildcard tests/*.c)
TEST_BINS := $(patsubst tests/%.c,$(BUILD)/tests/%,$(SRC_TESTS))
DEPS += $(patsubst %.o,%.d,$(call objs,$(SRC_TESTS)))

$(BUILD)/tests/%: $(OBJDIR)/tests/%.o $(call objs,$(SRC_COMMON))
	@mkdir -p -- $(dir $@)
	$(CC) $(LDFLAGS_TARGET) -o $@ $^ -lm -lpthread

test: $(TEST_BINS)
	@if [ -z "$(TEST_BINS)" ]; then printf '%s\n' 'no tests'; fi
	@for T in $(TEST_BINS); do printf 'run %s\n' "$$T"; "$$T" || exit 1; done

apk:
	$(error apk is only available for TARGET=android-arm64)

#
# Host preview of the operating screen (made-up data, writes a PPM)
#

C_PREVIEW := $(BUILD)/ui-preview
DEPS += $(patsubst %.o,%.d,$(call objs,tools/ui_preview.c $(SRC_UI)))

$(OBJDIR)/tools/ui_preview.o: tools/ui_preview.c
	@mkdir -p -- $(dir $@)
	$(CC) $(CFLAGS_COMMON) $(C_UI_FLAGS) $(CFLAGS_TARGET) -c -o $@ $<

$(C_PREVIEW): $(call objs,tools/ui_preview.c $(SRC_UI) $(SRC_COMMON))
	$(CC) $(LDFLAGS_TARGET) -o $@ $^ -lm -lpthread

preview: $(C_PREVIEW)
