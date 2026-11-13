// SPDX-License-Identifier: GPL-3.0-or-later
//
// Icom scope frame assembler: Scope Wave Data (27 00) sequences to rows.
//
// Port of IcomScopeFrameAssembler::parsePayload from wfview main
// src/radio/icomscopeframeassembler.cpp,
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "model/radio_state.h"

struct scope_asm {
	uint8_t next_seq;
	uint8_t old_mode;
	bool have_freq;
	uint8_t mode;
	uint64_t start_hz;
	uint64_t end_hz;
	size_t len;
	uint8_t data[RADIO_SCOPE_MAX_BINS];
};

//
// Assembly
//

void scope_asm_reset(struct scope_asm *a);
bool scope_asm_parse(struct scope_asm *a, const uint8_t *payload, size_t len, int model_id,
		size_t expected_len, struct radio_scope_row *out);
