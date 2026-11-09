# SPDX-License-Identifier: GPL-3.0-or-later
#
# Common flags, source lists and compile rule for all targets
#

C_STD_FLAGS  := -std=c11 -D_POSIX_C_SOURCE=200809L
C_WARN_FLAGS := -Wall -Wextra -Werror -Wshadow -Wconversion

ifeq ($(DEBUG),1)
C_OPT_FLAGS := -O0 -g3
else
C_OPT_FLAGS := -O2 -g -DNDEBUG
endif

CFLAGS_COMMON := $(C_STD_FLAGS) $(C_WARN_FLAGS) $(C_OPT_FLAGS) -MMD -MP -I.

OBJDIR := $(BUILD)/obj

include mk/third_party.mk

SRC_COMMON := $(wildcard core/*.c) $(wildcard model/*.c) $(SRC_THIRD_PARTY)

# Operating screen: own code plus LVGL, linked into the platform programs only
SRC_UI := $(wildcard ui/*.c) $(SRC_LVGL)
C_UI_FLAGS := -DLV_CONF_INCLUDE_SIMPLE -isystem third_party/lvgl -Iui

# Objects for a list of sources: build/<target>/obj/<path>.o
objs = $(patsubst %.c,$(OBJDIR)/%.o,$(1))

$(OBJDIR)/%.o: %.c
	@mkdir -p -- $(dir $@)
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_TARGET) -c -o $@ $<

$(OBJDIR)/ui/%.o: ui/%.c
	@mkdir -p -- $(dir $@)
	$(CC) $(CFLAGS_COMMON) $(C_UI_FLAGS) $(CFLAGS_TARGET) -c -o $@ $<
