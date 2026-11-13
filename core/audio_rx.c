// SPDX-License-Identifier: GPL-3.0-or-later
//
// RX audio pipeline: decode, volume, mono to L+R, resample, output buffer.
//
// Behaviour as wfview main audioconverter.cpp / audiohandlerqtoutput.cpp,
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//

#include "core/audio_rx.h"

#include <math.h>
#include <string.h>

#include "core/audio_taper.h"

#define C_RING_MASK	(AUDIO_RX_RING_FRAMES - 1)

static size_t ring_fill(const struct audio_rx *a)
{
	return atomic_load_explicit(&a->head, memory_order_acquire) -
		atomic_load_explicit(&a->tail, memory_order_acquire);
}

//
// Write frames (stereo) into the ring, at most up to the target size.
// Returns frames written; the rest is dropped, as a full Qt sink drops it.
//

static size_t ring_write(struct audio_rx *a, const float *src, size_t frames)
{
	const size_t fill = ring_fill(a);
	size_t head = atomic_load_explicit(&a->head, memory_order_relaxed);
	size_t room = a->target_frames > fill ? a->target_frames - fill : 0;
	size_t i;

	if (frames > room)
		frames = room;

	for (i = 0; i < frames; i++) {
		const size_t idx = (head + i) & C_RING_MASK;

		if (src != NULL) {
			a->ring[idx * 2] = src[i * 2];
			a->ring[idx * 2 + 1] = src[i * 2 + 1];
		} else {
			a->ring[idx * 2] = 0.0f;
			a->ring[idx * 2 + 1] = 0.0f;
		}
	}

	atomic_store_explicit(&a->head, head + frames, memory_order_release);
	return frames;
}

void audio_rx_init(struct audio_rx *a, uint32_t dev_rate)
{
	memset(a, 0, sizeof(*a));
	a->dev_rate = dev_rate;
	atomic_init(&a->volume, 255u);
	atomic_init(&a->underrun, false);
	atomic_init(&a->head, 0);
	atomic_init(&a->tail, 0);
	atomic_init(&a->underruns, 0u);
}

//
// (Re)configure for a stream; pre-fills half the target with silence
//

int audio_rx_configure(struct audio_rx *a, uint8_t codec, uint32_t rx_rate, uint16_t latency_ms)
{
	if (a->configured)
		audio_rx_stop(a);

	if (audio_decoder_init(&a->dec, codec, rx_rate) != 0)
		return -1;
	if (resampler_init(&a->rs, rx_rate, a->dev_rate, 2) != 0) {
		audio_decoder_free(&a->dec);
		return -1;
	}

	a->latency_ms = latency_ms;
	a->target_frames = (size_t)a->dev_rate * latency_ms / 1000;
	if (a->target_frames > AUDIO_RX_RING_FRAMES)
		a->target_frames = AUDIO_RX_RING_FRAMES;
	a->last_write_ms = 0;
	memset(&a->st, 0, sizeof(a->st));

	ring_write(a, NULL, a->target_frames / 2);
	atomic_store(&a->underrun, false);
	a->configured = true;
	return 0;
}

void audio_rx_stop(struct audio_rx *a)
{
	if (!a->configured)
		return;
	audio_decoder_free(&a->dec);
	a->configured = false;
}

void audio_rx_set_volume(struct audio_rx *a, uint8_t volume)
{
	atomic_store(&a->volume, volume);
}

//
// One received packet
//

void audio_rx_push(struct audio_rx *a, const uint8_t *payload, size_t len, int64_t arrival_ms, int64_t now_ms)
{
	const float gain = AUDIO_TAPER[atomic_load(&a->volume) & 0xffu];
	const int ch = a->dec.channels;
	size_t frames;
	size_t out_frames;
	size_t i;
	float peak = 0.0f;
	double sq = 0.0;
	int n;

	if (!a->configured || len == 0)
		return;

	// Output-side age check (audiohandlerqtoutput onConverted)
	if ((double)(now_ms - arrival_ms) > a->latency_ms * 1.5) {
		a->st.dropped_old++;
		return;
	}

	n = audio_decode(&a->dec, payload, len, a->decoded, AUDIO_DECODE_MAX_FRAMES);
	if (n <= 0) {
		if (n < 0)
			a->st.decode_errors++;
		return;
	}
	frames = (size_t)n;

	// Levels before volume, on the samples as received
	for (i = 0; i < frames * (size_t)ch; i++) {
		const float v = fabsf(a->decoded[i]);

		if (v > peak)
			peak = v;
		sq += (double)a->decoded[i] * a->decoded[i];
	}
	a->st.peak = peak;
	a->st.rms = (float)sqrt(sq / (double)(frames * (size_t)ch));

	// Volume, then channel conversion to stereo
	for (i = 0; i < frames; i++) {
		if (ch == 1) {
			const float v = a->decoded[i] * gain;

			a->stereo[i * 2] = v;
			a->stereo[i * 2 + 1] = v;
		} else {
			a->stereo[i * 2] = a->decoded[i * 2] * gain;
			a->stereo[i * 2 + 1] = a->decoded[i * 2 + 1] * gain;
		}
	}

	out_frames = resampler_process(&a->rs, a->stereo, frames, a->resampled,
			sizeof(a->resampled) / sizeof(a->resampled[0]) / 2);

	// Underrun recovery: re-prime with up to half the buffer of silence
	if (atomic_exchange(&a->underrun, false)) {
		const size_t fill = ring_fill(a);
		const size_t room = a->target_frames > fill ? a->target_frames - fill : 0;
		const size_t half = a->target_frames / 2;

		ring_write(a, NULL, half < room ? half : room);
	}

	// Latency estimate: 0.8 previous + 0.2 (time since last write + buffered)
	{
		const int pipeline = a->last_write_ms != 0 ? (int)(now_ms - a->last_write_ms) : 0;
		const int dev = (int)(ring_fill(a) * 1000 / a->dev_rate);

		a->st.latency_ms = (int)((double)a->st.latency_ms * 0.8 + (double)(pipeline + dev) * 0.2);
	}

	i = ring_write(a, a->resampled, out_frames);
	if (i < out_frames)
		a->st.overrun_frames += (uint32_t)(out_frames - i);

	a->last_write_ms = now_ms;
}

void audio_rx_get_stats(const struct audio_rx *a, struct audio_rx_stats *out)
{
	*out = a->st;
	out->underruns = atomic_load(&a->underruns);
}

//
// Device callback: copy out, silence on underrun (no allocation, no locks)
//

void audio_rx_read(struct audio_rx *a, float *out, size_t frames)
{
	const size_t head = atomic_load_explicit(&a->head, memory_order_acquire);
	size_t tail = atomic_load_explicit(&a->tail, memory_order_relaxed);
	const size_t avail = head - tail;
	const size_t n = frames < avail ? frames : avail;
	size_t i;

	for (i = 0; i < n; i++) {
		const size_t idx = (tail + i) & C_RING_MASK;

		out[i * 2] = a->ring[idx * 2];
		out[i * 2 + 1] = a->ring[idx * 2 + 1];
	}
	for (; i < frames; i++) {
		out[i * 2] = 0.0f;
		out[i * 2 + 1] = 0.0f;
	}

	atomic_store_explicit(&a->tail, tail + n, memory_order_release);

	if (n < frames && !atomic_load_explicit(&a->underrun, memory_order_relaxed)) {
		atomic_store_explicit(&a->underrun, true, memory_order_relaxed);
		atomic_fetch_add_explicit(&a->underruns, 1u, memory_order_relaxed);
	}
}
