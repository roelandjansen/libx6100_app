// SPDX-License-Identifier: GPL-3.0-or-later
//
// TX audio pipeline: capture ring, mic gain, mono, resample, encode.
//
// Conversions and encoder settings as wfview v3-initial
// src/audio/audioconverter.cpp,
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//

#include "core/tx_audio.h"

#include <math.h>
#include <string.h>

#include "core/audio_decode.h"
#include "third_party/adpcm/adpcm-lib.h"
#include "third_party/opus/include/opus.h"

#define C_RING_MASK	(TX_AUDIO_RING_FRAMES - 1u)

void tx_audio_init(struct tx_audio *t, uint32_t cap_rate)
{
	memset(t, 0, sizeof(*t));
	t->cap_rate = cap_rate;
	atomic_init(&t->gain_pct, 100u);
	atomic_init(&t->head, 0);
	atomic_init(&t->tail, 0);
	atomic_init(&t->overrun_frames, 0u);
}

static void free_encoders(struct tx_audio *t)
{
	if (t->opus != NULL)
		opus_encoder_destroy(t->opus);
	t->opus = NULL;
	if (t->adpcm != NULL)
		adpcm_free_context(t->adpcm);
	t->adpcm = NULL;
}

//
// Configure for the negotiated TX codec and rate. Icom LAN TX audio is
// mono: a 2-channel codec is refused (wfview drops such packets).
//

int tx_audio_configure(struct tx_audio *t, uint8_t codec, uint32_t tx_rate)
{
	free_encoders(t);
	t->configured = false;
	t->fifo_n = 0;

	if (t->cap_rate == 0 || tx_rate == 0 || tx_rate > 48000 || t->cap_rate > 48000)
		return TX_AUDIO_ERR_RATE;

	switch (codec) {
	case AUDIO_CODEC_ULAW_1:
	case AUDIO_CODEC_PCM8_1:
	case AUDIO_CODEC_PCM16_1:
		break;
	case AUDIO_CODEC_OPUS_1: {
		int err = 0;
		OpusEncoder *enc;

		if (tx_rate != 8000 && tx_rate != 12000 && tx_rate != 16000 &&
				tx_rate != 24000 && tx_rate != 48000)
			return TX_AUDIO_ERR_RATE;
		enc = opus_encoder_create((opus_int32)tx_rate, 1, OPUS_APPLICATION_AUDIO, &err);
		if (enc == NULL || err != OPUS_OK)
			return TX_AUDIO_ERR_ENCODER;
		opus_encoder_ctl(enc, OPUS_SET_BITRATE(TX_AUDIO_OPUS_BITRATE));
		opus_encoder_ctl(enc, OPUS_SET_VBR(1));
		opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
		opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(TX_AUDIO_OPUS_COMPLEXITY));
		t->opus = enc;
		break;
	}
	case AUDIO_CODEC_ADPCM_1:
		// adpcm_create_context(channels, rate, lookahead 1, noise shaping 1) as wfview
		t->adpcm = adpcm_create_context(1, (int)tx_rate, 1, 1);
		if (t->adpcm == NULL)
			return TX_AUDIO_ERR_ENCODER;
		break;
	default:
		return TX_AUDIO_ERR_CODEC;
	}

	if (t->cap_rate != tx_rate && resampler_init(&t->rs, t->cap_rate, tx_rate, 1) != 0) {
		free_encoders(t);
		return TX_AUDIO_ERR_RATE;
	}

	t->codec = codec;
	t->tx_rate = tx_rate;
	t->block_in = t->cap_rate * TX_AUDIO_BLOCK_MS / 1000;
	t->block_out = tx_rate * TX_AUDIO_BLOCK_MS / 1000;
	memset(&t->st, 0, sizeof(t->st));
	t->configured = true;
	return 0;
}

void tx_audio_stop(struct tx_audio *t)
{
	free_encoders(t);
	t->configured = false;
	t->fifo_n = 0;
}

//
// Capture callback: copy into the ring, drop what does not fit
//

void tx_audio_write(struct tx_audio *t, const float *in, size_t frames, int channels)
{
	size_t head;
	size_t tail;
	size_t space;
	const size_t step = channels > 1 ? (size_t)channels : 1u;
	size_t n = frames;
	size_t i;

	if (t == NULL)
		return;
	head = atomic_load_explicit(&t->head, memory_order_relaxed);
	tail = atomic_load_explicit(&t->tail, memory_order_acquire);
	space = TX_AUDIO_RING_FRAMES - (head - tail);
	if (n > space) {
		atomic_fetch_add_explicit(&t->overrun_frames, (unsigned int)(n - space), memory_order_relaxed);
		n = space;
	}

	for (i = 0; i < n; i++)
		t->ring[(head + i) & C_RING_MASK] = in[i * step];

	atomic_store_explicit(&t->head, head + n, memory_order_release);
}

//
// Network thread: throw away what was captured (not keyed / start of TX)
//

void tx_audio_discard(struct tx_audio *t)
{
	atomic_store_explicit(&t->tail, atomic_load_explicit(&t->head, memory_order_acquire),
			memory_order_release);
	t->fifo_n = 0;
	if (t->configured && t->cap_rate != t->tx_rate)
		resampler_reset(&t->rs);
}

uint8_t tx_audio_ulaw(float x)
{
	// ITU G.711 u-law on 16-bit samples (bias 0x84, clip 32635). wfview
	// audioconverter.cpp uses the 13-bit segment ends {0x1f .. 0xfff} here,
	// which pushes most samples into the top segments; this uses the 16-bit
	// ends, so the output matches the u-law decoder (deviation 7).
	static const int seg_end[8] = { 0xff, 0x1ff, 0x3ff, 0x7ff, 0xfff, 0x1fff, 0x3fff, 0x7fff };
	const int bias = 0x84;
	const int clip = 32635;
	int s;
	int pcm;
	int mask;
	int seg = 0;
	int mant;

	if (x > 1.0f)
		x = 1.0f;
	if (x < -1.0f)
		x = -1.0f;
	s = (int)lrintf(x * 32767.0f);

	pcm = s;
	if (pcm < 0) {
		pcm = bias - pcm;
		mask = 0x7f;
	} else {
		pcm = bias + pcm;
		mask = 0xff;
	}
	if (pcm > bias + clip)
		pcm = bias + clip;

	while (seg < 8 && pcm > seg_end[seg])
		seg++;
	if (seg > 7)
		seg = 7;

	mant = (pcm >> (seg + 3)) & 0x0f;
	return (uint8_t)(((seg << 4) | mant) ^ mask);
}

static float clampf(float x)
{
	return x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
}

//
// Encode one block of block_out mono samples; returns bytes or < 0
//

static int encode(struct tx_audio *t, const float *x, size_t n)
{
	size_t i;

	switch (t->codec) {
	case AUDIO_CODEC_OPUS_1:
		return opus_encode_float(t->opus, x, (int)n, t->enc, (opus_int32)sizeof(t->enc));
	case AUDIO_CODEC_ULAW_1:
		for (i = 0; i < n; i++)
			t->enc[i] = tx_audio_ulaw(x[i]);
		return (int)n;
	case AUDIO_CODEC_PCM8_1:
		for (i = 0; i < n; i++)
			t->enc[i] = (uint8_t)lrintf(clampf(x[i]) * 127.0f + 128.0f);
		return (int)n;
	case AUDIO_CODEC_PCM16_1:
		for (i = 0; i < n; i++) {
			const int16_t v = (int16_t)lrintf(clampf(x[i]) * 32767.0f);

			t->enc[i * 2] = (uint8_t)((uint16_t)v & 0xffu);
			t->enc[i * 2 + 1] = (uint8_t)((uint16_t)v >> 8);
		}
		return (int)(n * 2);
	case AUDIO_CODEC_ADPCM_1: {
		int16_t s16[TX_AUDIO_MAX_BLOCK];
		size_t out = 0;

		for (i = 0; i < n; i++)
			s16[i] = (int16_t)lrintf(clampf(x[i]) * 32767.0f);
		if (!adpcm_encode_block(t->adpcm, t->enc, &out, s16, (int)n))
			return -1;
		return (int)out;
	}
	default:
		return -1;
	}
}

//
// Network thread: turn every complete 20 ms capture block into encoded
// blocks. Returns the number of blocks emitted.
//

size_t tx_audio_pump(struct tx_audio *t, tx_audio_emit emit, void *ctx)
{
	size_t emitted = 0;

	if (!t->configured)
		return 0;

	for (;;) {
		const size_t head = atomic_load_explicit(&t->head, memory_order_acquire);
		size_t tail = atomic_load_explicit(&t->tail, memory_order_relaxed);
		const float gain = (float)atomic_load(&t->gain_pct) / 100.0f;
		const float *src;
		size_t got;
		size_t i;

		if (head - tail < t->block_in)
			break;

		for (i = 0; i < t->block_in; i++)
			t->in[i] = t->ring[(tail + i) & C_RING_MASK] * gain;
		tail += t->block_in;
		atomic_store_explicit(&t->tail, tail, memory_order_release);

		if (t->cap_rate != t->tx_rate) {
			got = resampler_process(&t->rs, t->in, t->block_in, t->res,
					sizeof(t->res) / sizeof(t->res[0]));
			src = t->res;
		} else {
			got = t->block_in;
			src = t->in;
		}

		if (t->fifo_n + got > sizeof(t->fifo) / sizeof(t->fifo[0]))
			t->fifo_n = 0;
		memcpy(t->fifo + t->fifo_n, src, got * sizeof(float));
		t->fifo_n += got;

		while (t->fifo_n >= t->block_out) {
			float peak = 0.0f;
			double sq = 0.0;
			int n;

			for (i = 0; i < t->block_out; i++) {
				const float a = fabsf(t->fifo[i]);

				if (a > peak)
					peak = a;
				sq += (double)t->fifo[i] * t->fifo[i];
			}

			n = encode(t, t->fifo, t->block_out);
			t->fifo_n -= t->block_out;
			memmove(t->fifo, t->fifo + t->block_out, t->fifo_n * sizeof(float));

			t->st.peak = peak;
			t->st.rms = (float)sqrt(sq / (double)t->block_out);
			if (n < 0) {
				t->st.encode_errors++;
				continue;
			}
			t->st.blocks++;
			emitted++;
			if (emit != NULL && n > 0)
				emit(ctx, t->enc, (size_t)n, t->st.peak, t->st.rms);
		}
	}

	return emitted;
}

void tx_audio_set_gain(struct tx_audio *t, unsigned int percent)
{
	atomic_store(&t->gain_pct, percent > 400u ? 400u : percent);
}

void tx_audio_get_stats(const struct tx_audio *t, struct tx_audio_stats *out)
{
	*out = t->st;
	out->overrun_frames = atomic_load(&t->overrun_frames);
}
