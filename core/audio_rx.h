// SPDX-License-Identifier: GPL-3.0-or-later
//
// RX audio pipeline: decode, volume, mono to L+R, resample, output buffer.
//
// Writer side runs on the session network thread, reader side in the
// audio device callback. The ring is lock-free single-producer /
// single-consumer and keeps its storage for the session lifetime, so a
// stream reconfiguration on the writer side never races the reader.
//
// Output buffer behaviour as wfview main src/audio/audiohandlerqtoutput.cpp:
// target size = latency, half pre-filled with silence on start, refilled
// with up to half on the first write after an underrun, late packets
// (> 1.5 x latency since arrival) dropped,
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//

#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/audio_decode.h"
#include "core/resample.h"

#define AUDIO_RX_RING_FRAMES	(1u << 17)	// 2.7 s at 48 kHz, power of two

struct audio_rx_stats {
	float peak;
	float rms;
	int latency_ms;
	uint32_t underruns;
	uint32_t overrun_frames;
	uint32_t dropped_old;
	uint32_t decode_errors;
};

struct audio_rx {

	//
	// Writer side (network thread)
	//
	bool configured;
	struct audio_decoder dec;
	struct resampler rs;
	uint32_t dev_rate;
	uint16_t latency_ms;
	size_t target_frames;
	int64_t last_write_ms;
	float decoded[AUDIO_DECODE_MAX_FRAMES * 2];
	float stereo[AUDIO_DECODE_MAX_FRAMES * 2];
	float resampled[AUDIO_DECODE_MAX_FRAMES * 2 * 8];

	//
	// Shared
	//
	atomic_uint volume;		// 0..255 index into AUDIO_TAPER
	atomic_bool underrun;
	atomic_size_t head;		// written frames (writer)
	atomic_size_t tail;		// read frames (reader)
	atomic_uint underruns;
	float ring[AUDIO_RX_RING_FRAMES * 2];

	//
	// Stats (writer side)
	//
	struct audio_rx_stats st;
};

//
// Lifecycle (network thread)
//

void audio_rx_init(struct audio_rx *a, uint32_t dev_rate);
int audio_rx_configure(struct audio_rx *a, uint8_t codec, uint32_t rx_rate, uint16_t latency_ms);
void audio_rx_stop(struct audio_rx *a);

//
// Writer (network thread)
//

void audio_rx_push(struct audio_rx *a, const uint8_t *payload, size_t len, int64_t arrival_ms, int64_t now_ms);
void audio_rx_get_stats(const struct audio_rx *a, struct audio_rx_stats *out);

//
// Reader (device callback): always fills frames stereo floats
//

void audio_rx_read(struct audio_rx *a, float *out, size_t frames);

//
// Controls (any thread)
//

void audio_rx_set_volume(struct audio_rx *a, uint8_t volume);
