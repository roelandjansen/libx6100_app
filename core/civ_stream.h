// SPDX-License-Identifier: GPL-3.0-or-later
//
// CI-V UDP stream: open/close, CI-V data in both directions, watchdog.
//
// Port of icomUdpCivData from wfview v3-initial src/radio/icomudpcivdata.cpp,
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/udp_stream.h"

#define CIV_DATA_HEADER		0x15
#define CIV_WATCHDOG_MS		2000
#define CIV_OPEN_REPEAT_MS	100

struct civ_stream {

	//
	// Stream and output
	//
	struct udp_stream s;
	void (*on_data)(void *ctx, const uint8_t *civ, size_t len);
	void *ctx;

	//
	// CI-V inner sequence (big endian on the wire)
	//
	uint16_t send_seq_b;

	//
	// Open repeat and watchdog
	//
	int64_t t_open;
	int64_t t_watchdog;
	int64_t last_data;
	bool alerted;

	uint32_t frames_rx;
};

//
// Lifecycle
//

void civ_stream_init(struct civ_stream *c, const struct udp_io *io, uint32_t my_id,
		void (*on_data)(void *ctx, const uint8_t *civ, size_t len), void *ctx, int64_t now_ms);
void civ_stream_close(struct civ_stream *c, int64_t now_ms);

//
// Traffic and timers
//

void civ_stream_receive(struct civ_stream *c, const uint8_t *buf, size_t len, int64_t now_ms);
int civ_stream_send(struct civ_stream *c, const uint8_t *civ, size_t len, int64_t now_ms);
void civ_stream_tick(struct civ_stream *c, int64_t now_ms);
int64_t civ_stream_next_deadline(const struct civ_stream *c);
