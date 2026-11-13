// SPDX-License-Identifier: GPL-3.0-or-later
//
// Icom scope frame assembler: Scope Wave Data (27 00) sequences to rows.
//
// Port of IcomScopeFrameAssembler::parsePayload from wfview main
// src/radio/icomscopeframeassembler.cpp,
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//
// payload starts after the receiver byte: [seq BCD][seq max BCD] ...
// Frequencies are kept in Hz (wfview converts to MHz double).
//

#include "core/scope.h"

#include <string.h>

static uint8_t bcd(uint8_t in)
{
	return (uint8_t)((in & 0x0fu) + ((in & 0xf0u) >> 4) * 10u);
}

static uint64_t bcd_freq(const uint8_t *p, size_t n)
{
	uint64_t v = 0;
	uint64_t m = 1;
	size_t i;

	for (i = 0; i < n; i++) {
		v += (uint64_t)(p[i] & 0x0fu) * m;
		m *= 10;
		v += (uint64_t)((p[i] & 0xf0u) >> 4) * m;
		m *= 10;
	}

	return v;
}

void scope_asm_reset(struct scope_asm *a)
{
	memset(a, 0, sizeof(*a));
}

static void append(struct scope_asm *a, const uint8_t *p, size_t n)
{
	if (a->len + n > sizeof(a->data))
		n = sizeof(a->data) - a->len;
	memcpy(a->data + a->len, p, n);
	a->len += n;
}

static void drop(struct scope_asm *a)
{
	a->len = 0;
	a->next_seq = 0;
}

//
// Feed one Scope Wave Data payload; true and *out filled when a row is complete
//

bool scope_asm_parse(struct scope_asm *a, const uint8_t *payload, size_t len, int model_id,
		size_t expected_len, struct radio_scope_row *out)
{
	uint8_t seq;
	uint8_t seq_max;
	size_t freq_len = 5;
	bool ret = false;
	bool oor = false;

	if (len < 2)
		return false;

	seq = bcd(payload[0]);
	seq_max = bcd(payload[1]);
	if (seq == 0 || seq_max == 0 || seq > seq_max) {
		drop(a);
		return false;
	}

	if (model_id == 0xac && (len == 491 || len == 16))
		freq_len = 6;

	if (seq == 1) {
		const size_t info = 4 + freq_len * 2;

		if (len < info) {
			drop(a);
			return false;
		}

		a->mode = payload[2];
		if (a->mode != a->old_mode)
			a->old_mode = a->mode;

		oor = payload[3 + freq_len * 2] != 0;
		if (oor) {
			out->valid = true;
			out->oor = true;
			out->low_hz = a->start_hz;
			out->high_hz = a->end_hz;
			out->len = (uint16_t)(expected_len < RADIO_SCOPE_MAX_BINS ? expected_len : RADIO_SCOPE_MAX_BINS);
			memset(out->bins, 0, out->len);
			a->next_seq = 0;
			return true;
		}

		a->len = 0;
		a->next_seq = (seq < seq_max) ? (uint8_t)(seq + 1) : 0;

		a->start_hz = bcd_freq(payload + 3, freq_len);
		a->end_hz = bcd_freq(payload + 3 + freq_len, freq_len);
		a->have_freq = true;

		if (a->mode == 0) {
			// Center mode: A = center, B = half span
			const uint64_t half = a->end_hz;

			a->start_hz = a->start_hz > half ? a->start_hz - half : 0;
			a->end_hz = a->start_hz + 2 * half;
		}

		if (seq == seq_max) {
			append(a, payload + info, len - info);
			ret = true;
		}
	} else {
		if (!a->have_freq || a->start_hz == 0 || a->end_hz <= a->start_hz || a->next_seq != seq) {
			drop(a);
			return false;
		}

		append(a, payload + 2, len - 2);
		if (seq < seq_max) {
			a->next_seq = (uint8_t)(seq + 1);
		} else {
			ret = true;
			a->next_seq = 0;
		}
	}

	if (ret && expected_len > 0 && a->len != expected_len) {
		drop(a);
		return false;
	}

	if (ret) {
		out->valid = true;
		out->oor = false;
		out->low_hz = a->start_hz;
		out->high_hz = a->end_hz;
		out->len = (uint16_t)a->len;
		memcpy(out->bins, a->data, a->len);
	}

	return ret;
}
