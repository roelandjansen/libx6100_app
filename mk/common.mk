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

SRC_COMMON := $(wildcard core/*.c) $(wildcard model/*.c) $(wildcard ui/*.c)

OBJDIR := $(BUILD)/obj

# Objects for a list of sources: build/<target>/obj/<path>.o
objs = $(patsubst %.c,$(OBJDIR)/%.o,$(1))

$(OBJDIR)/%.o: %.c
	@mkdir -p -- $(dir $@)
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_TARGET) -c -o $@ $<
