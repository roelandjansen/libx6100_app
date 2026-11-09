// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audio UDP stream: RX and TX audio packets, late-packet drop, 30 s watchdog.
// Decoding and playback are phase 2; this delivers the payload.
//
// Port of the stream part of icomUdpAudio from wfview v3-initial
// src/radio/icomudpaudio.cpp,
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/udp_stream.h"

#define AUDIO_HEADER		0x18
#define AUDIO_WATCHDOG_MS	30000

//
// Audio packet header fields (wfview packettypes.h audio_packet)
//

#define AUDIO_OFF_IDENT		0x10
#define AUDIO_OFF_SENDSEQ	0x12
#define AUDIO_OFF_DATALEN	0x16

struct audio_rx_events {

	//
	// Player start / stop (wfview starts on first packet, stops on the watchdog)
	//
	void (*start)(void *ctx);
	void (*stop)(void *ctx);

	//
	// One audio payload, extended 32-bit sequence
	//
	void (*data)(void *ctx, uint32_t seq, const uint8_t *payload, size_t len);

	void *ctx;
};

struct audio_stream {

	//
	// Stream and output
	//
	struct udp_stream s;
	struct audio_rx_events ev;
	uint16_t rx_latency_ms;

	//
	// RX state
	//
	uint16_t seq_prefix;
	bool playing;
	int64_t last_audio;
	int64_t t_watchdog;
	bool alerted;

	//
	// Stats
	//
	uint32_t packets_rx;
	uint32_t dropped_late;

	//
	// TX (sendAudioSeq and counters)
	//
	uint16_t tx_audio_seq;
	uint32_t packets_tx;
	uint32_t bytes_tx;
};

//
// Lifecycle
//

void audio_stream_init(struct audio_stream *a, const struct udp_io *io, uint32_t my_id,
		const struct audio_rx_events *ev, uint16_t rx_latency_ms, int64_t now_ms);
void audio_stream_close(struct audio_stream *a);

//
// Traffic and timers
//

void audio_stream_receive(struct audio_stream *a, const uint8_t *buf, size_t len, int64_t now_ms);
void audio_stream_tick(struct audio_stream *a, int64_t now_ms);
int64_t audio_stream_next_deadline(const struct audio_stream *a);

//
// TX audio: one encoded block, split into tracked packets of at most 1364 bytes
//

#define AUDIO_TX_CHUNK		1364

int audio_stream_send_tx(struct audio_stream *a, const uint8_t *data, size_t len, int64_t now_ms);
