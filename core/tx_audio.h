// SPDX-License-Identifier: GPL-3.0-or-later
//
// TX audio pipeline: capture ring, mic gain, mono, resample, encode.
//
// Writer side runs in the capture device callback, reader side on the
// session network thread. The ring is lock-free single-producer /
// single-consumer and keeps its storage for the session lifetime.
//
// Conversions and encoder settings as wfview main
// src/audio/audioconverter.cpp (20 ms blocks, Opus 64 kbit/s VBR voice,
// G.711 u-law, unsigned 8 bit, signed 16 bit, adpcm-xq),
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//

#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/resample.h"

#define TX_AUDIO_RING_FRAMES	(1u << 15)	// 0.68 s at 48 kHz, power of two
#define TX_AUDIO_BLOCK_MS	20		// wfview audioSetup.blockSize
#define TX_AUDIO_MAX_BLOCK	960		// 20 ms at 48 kHz
#define TX_AUDIO_MAX_ENC	(TX_AUDIO_MAX_BLOCK * 2)
#define TX_AUDIO_OPUS_BITRATE	64000
#define TX_AUDIO_OPUS_COMPLEXITY 7

#define TX_AUDIO_ERR_CODEC	(-1)
#define TX_AUDIO_ERR_RATE	(-2)
#define TX_AUDIO_ERR_ENCODER	(-3)

struct tx_audio_stats {
	float peak;
	float rms;
	uint32_t blocks;
	uint32_t encode_errors;
	uint32_t overrun_frames;
};

//
// One encoded block, delivered to the caller (network thread)
//

typedef void (*tx_audio_emit)(void *ctx, const uint8_t *data, size_t len, float peak, float rms);

struct tx_audio {

	//
	// Reader side (network thread)
	//
	bool configured;
	uint8_t codec;
	uint32_t cap_rate;
	uint32_t tx_rate;
	size_t block_in;
	size_t block_out;
	struct resampler rs;
	void *opus;
	void *adpcm;
	size_t fifo_n;
	float in[TX_AUDIO_RING_FRAMES / 8];
	float res[TX_AUDIO_RING_FRAMES / 4];
	float fifo[TX_AUDIO_MAX_BLOCK * 8];
	uint8_t enc[TX_AUDIO_MAX_ENC];

	//
	// Shared
	//
	atomic_uint gain_pct;
	atomic_size_t head;		// written frames (capture)
	atomic_size_t tail;		// read frames (network thread)
	atomic_uint overrun_frames;
	float ring[TX_AUDIO_RING_FRAMES];

	//
	// Stats (reader side)
	//
	struct tx_audio_stats st;
};

//
// Lifecycle (network thread)
//

void tx_audio_init(struct tx_audio *t, uint32_t cap_rate);
int tx_audio_configure(struct tx_audio *t, uint8_t codec, uint32_t tx_rate);
void tx_audio_stop(struct tx_audio *t);

//
// Writer (capture callback): channels 1 or 2, first channel taken as wfview
//

void tx_audio_write(struct tx_audio *t, const float *in, size_t frames, int channels);

//
// Reader (network thread)
//

void tx_audio_discard(struct tx_audio *t);
size_t tx_audio_pump(struct tx_audio *t, tx_audio_emit emit, void *ctx);

//
// Settings and stats (any thread / network thread)
//

void tx_audio_set_gain(struct tx_audio *t, unsigned int percent);
void tx_audio_get_stats(const struct tx_audio *t, struct tx_audio_stats *out);

//
// Encoders, exposed for tests
//

uint8_t tx_audio_ulaw(float x);
