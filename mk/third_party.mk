# SPDX-License-Identifier: GPL-3.0-or-later
#
# Vendored third-party code, built from source for every target with its
# own flags (no -Werror / -Wconversion: not our code, kept unmodified).
#
#	third_party/opus   libopus 1.5.2, BSD (see third_party/opus/COPYING), float build, no DNN
#	third_party/adpcm  adpcm-xq (as in wfview src/audio/adpcm), BSD (see license.txt)
#	third_party/lvgl   LVGL 9.2.2 (commit 7f07a12), MIT (see LICENCE.txt), src/ unmodified
#	                   except src/libs/thorvg (C++, not used) left out
#

C_OPUS_DIR := third_party/opus

include $(C_OPUS_DIR)/silk_sources.mk
include $(C_OPUS_DIR)/celt_sources.mk
include $(C_OPUS_DIR)/opus_sources.mk

SRC_OPUS := $(addprefix $(C_OPUS_DIR)/,$(SILK_SOURCES) $(SILK_SOURCES_FLOAT) $(CELT_SOURCES) \
	$(OPUS_SOURCES) $(OPUS_SOURCES_FLOAT))

SRC_ADPCM := third_party/adpcm/adpcm-lib.c third_party/adpcm/adpcm-dns.c

SRC_THIRD_PARTY := $(SRC_OPUS) $(SRC_ADPCM)

SRC_LVGL := $(shell find third_party/lvgl/src -name '*.c' | sort)

C_OPUS_FLAGS := -DOPUS_BUILD -DUSE_ALLOCA -DHAVE_LRINTF -DHAVE_LRINT '-DPACKAGE_VERSION="1.5.2"' \
	-I$(C_OPUS_DIR)/include -I$(C_OPUS_DIR)/celt -I$(C_OPUS_DIR)/silk -I$(C_OPUS_DIR)/silk/float

ifeq ($(DEBUG),1)
C_TP_OPT := -O1 -g
else
C_TP_OPT := -O2 -g -DNDEBUG
endif

CFLAGS_THIRD_PARTY := -std=gnu11 -w $(C_TP_OPT) -MMD -MP

$(OBJDIR)/$(C_OPUS_DIR)/%.o: $(C_OPUS_DIR)/%.c
	@mkdir -p -- $(dir $@)
	$(CC) $(CFLAGS_THIRD_PARTY) $(C_OPUS_FLAGS) $(CFLAGS_TARGET_TP) -c -o $@ $<

$(OBJDIR)/third_party/adpcm/%.o: third_party/adpcm/%.c
	@mkdir -p -- $(dir $@)
	$(CC) $(CFLAGS_THIRD_PARTY) $(CFLAGS_TARGET_TP) -c -o $@ $<

$(OBJDIR)/third_party/lvgl/%.o: third_party/lvgl/%.c
	@mkdir -p -- $(dir $@)
	$(CC) $(CFLAGS_THIRD_PARTY) -DLV_CONF_INCLUDE_SIMPLE -Iui -Ithird_party/lvgl $(CFLAGS_TARGET_TP) -c -o $@ $<
