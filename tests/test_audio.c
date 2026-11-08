// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test: RX audio decoding, resampler, Opus and ADPCM round trips,
// output buffer pre-fill / underrun / overrun behaviour.
//

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/audio_decode.h"
#include "core/audio_rx.h"
#include "core/resample.h"
#include "third_party/adpcm/adpcm-lib.h"
#include "third_party/opus/include/opus.h"

static int G_FAILS;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		G_FAILS++; \
	} \
} while (0)

#define C_PI 3.14159265358979323846

static int near(double a, double b, double tol)
{
	return fabs(a - b) <= tol;
}

static void test_simple_codecs(void)
{
	struct audio_decoder d;
	float out[8];
	const uint8_t ulaw[3] = { 0xff, 0x00, 0x80 };
	const uint8_t pcm16[4] = { 0xff, 0x7f, 0x00, 0x80 };
	const uint8_t pcm8[3] = { 128, 255, 1 };

	// µ-law: 0xff -> 0, 0x00 -> -32124/32768, 0x80 -> +32124/32768
	CHECK(audio_decoder_init(&d, AUDIO_CODEC_ULAW_1, 8000) == 0);
	CHECK(audio_decode(&d, ulaw, 3, out, 8) == 3);
	CHECK(out[0] == 0.0f);
	CHECK(near(out[1], -32124.0 / 32768.0, 1e-6));
	CHECK(near(out[2], 32124.0 / 32768.0, 1e-6));
	audio_decoder_free(&d);

	// PCM16 LE: 32767 -> 1.0, -32768 -> -32768/32767
	CHECK(audio_decoder_init(&d, AUDIO_CODEC_PCM16_1, 48000) == 0);
	CHECK(audio_decode(&d, pcm16, 4, out, 8) == 2);
	CHECK(near(out[0], 1.0, 1e-6));
	CHECK(near(out[1], -32768.0 / 32767.0, 1e-6));
	audio_decoder_free(&d);

	// PCM16 stereo: 4 bytes = one frame
	CHECK(audio_decoder_init(&d, AUDIO_CODEC_PCM16_2, 48000) == 0);
	CHECK(d.channels == 2);
	CHECK(audio_decode(&d, pcm16, 4, out, 8) == 1);
	audio_decoder_free(&d);

	// PCM8 unsigned: (x - 128) / 127
	CHECK(audio_decoder_init(&d, AUDIO_CODEC_PCM8_1, 8000) == 0);
	CHECK(audio_decode(&d, pcm8, 3, out, 8) == 3);
	CHECK(out[0] == 0.0f);
	CHECK(near(out[1], 1.0, 1e-6));
	CHECK(near(out[2], -1.0, 1e-6));
	audio_decoder_free(&d);

	CHECK(audio_decoder_init(&d, 0x33, 8000) != 0);
}

//
// Frequency of a stereo signal's left channel from zero crossings
//

static double crossing_freq(const float *x, size_t frames, size_t skip, double rate)
{
	size_t i;
	size_t first = 0;
	size_t last = 0;
	int n = 0;

	for (i = skip + 1; i < frames; i++) {
		if (x[(i - 1) * 2] < 0.0f && x[i * 2] >= 0.0f) {
			if (n == 0)
				first = i;
			last = i;
			n++;
		}
	}

	if (n < 2)
		return 0.0;
	return (double)(n - 1) * rate / (double)(last - first);
}

static double peak_l(const float *x, size_t frames, size_t skip)
{
	double p = 0.0;
	size_t i;

	for (i = skip; i < frames; i++)
		if (fabs(x[i * 2]) > p)
			p = fabs(x[i * 2]);
	return p;
}

static void test_resampler(void)
{
	static struct resampler r;
	static float in[8000 * 2];
	static float out[48000 * 2 + 64];
	size_t n = 0;
	size_t i;

	// 1 kHz at 8 kHz up to 48 kHz, fed in 160-frame packets
	for (i = 0; i < 8000; i++) {
		const float v = 0.8f * (float)sin(2.0 * C_PI * 1000.0 * (double)i / 8000.0);

		in[i * 2] = v;
		in[i * 2 + 1] = v;
	}
	CHECK(resampler_init(&r, 8000, 48000, 2) == 0);
	for (i = 0; i < 8000; i += 160)
		n += resampler_process(&r, in + i * 2, 160, out + n * 2, 48000 + 32 - n);
	CHECK(n > 47900 && n <= 48000);
	CHECK(near(crossing_freq(out, n, 1000, 48000.0), 1000.0, 2.0));
	CHECK(near(peak_l(out, n, 1000), 0.8, 0.02));

	// 6 kHz at 48 kHz down to 8 kHz: above the new Nyquist, must be removed
	for (i = 0; i < 8000; i++) {
		const float v = 0.8f * (float)sin(2.0 * C_PI * 6000.0 * (double)i / 48000.0);

		in[i * 2] = v;
		in[i * 2 + 1] = v;
	}
	CHECK(resampler_init(&r, 48000, 8000, 2) == 0);
	n = resampler_process(&r, in, 8000, out, 48000);
	CHECK(n > 1300 && n <= 1334);
	CHECK(peak_l(out, n, 100) < 0.01);

	// Equal rates: pass through
	CHECK(resampler_init(&r, 48000, 48000, 2) == 0);
	CHECK(resampler_process(&r, in, 100, out, 1000) == 100);
	CHECK(out[10] == in[10]);
}

static void test_opus_roundtrip(void)
{
	struct audio_decoder d;
	OpusEncoder *enc;
	int err = 0;
	int f;
	double in_sq = 0.0;
	double out_sq = 0.0;
	int counted = 0;

	enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_AUDIO, &err);
	CHECK(enc != NULL && err == OPUS_OK);
	if (enc == NULL)
		return;
	opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000));
	CHECK(audio_decoder_init(&d, AUDIO_CODEC_OPUS_1, 48000) == 0);

	for (f = 0; f < 50; f++) {
		float pcm[960];
		float out[AUDIO_DECODE_MAX_FRAMES];
		uint8_t pkt[1500];
		int i;
		int len;
		int n;

		for (i = 0; i < 960; i++)
			pcm[i] = 0.5f * (float)sin(2.0 * C_PI * 440.0 * (double)(f * 960 + i) / 48000.0);

		len = opus_encode_float(enc, pcm, 960, pkt, sizeof(pkt));
		CHECK(len > 0);
		n = audio_decode(&d, pkt, (size_t)len, out, AUDIO_DECODE_MAX_FRAMES);
		CHECK(n == 960);

		if (f >= 10 && n == 960) {
			for (i = 0; i < 960; i++) {
				in_sq += (double)pcm[i] * pcm[i];
				out_sq += (double)out[i] * out[i];
			}
			counted++;
		}
	}

	CHECK(counted == 40);
	CHECK(near(sqrt(out_sq / in_sq), 1.0, 0.1));

	audio_decoder_free(&d);
	opus_encoder_destroy(enc);
}

static void test_adpcm_roundtrip(void)
{
	struct audio_decoder d;
	int16_t pcm[505];
	uint8_t blk[512];
	float out[AUDIO_DECODE_MAX_FRAMES];
	size_t blk_len = 0;
	void *ctx;
	double err = 0.0;
	int i;
	int n;

	for (i = 0; i < 505; i++)
		pcm[i] = (int16_t)(12000.0 * sin(2.0 * C_PI * 700.0 * i / 8000.0));

	ctx = adpcm_create_context(1, 8000, 1, 0);
	CHECK(ctx != NULL);
	adpcm_encode_block(ctx, blk, &blk_len, pcm, 505);
	adpcm_free_context(ctx);
	CHECK(blk_len == 256);

	CHECK(audio_decoder_init(&d, AUDIO_CODEC_ADPCM_1, 8000) == 0);
	n = audio_decode(&d, blk, blk_len, out, AUDIO_DECODE_MAX_FRAMES);
	// adpcm_decode_block() writes 505 samples but returns 504; wfview uses 504
	CHECK(n == 504);
	for (i = 0; i < n && i < 505; i++)
		err += fabs((double)out[i] * 32767.0 - pcm[i]);
	CHECK(err / 505.0 < 600.0);
	audio_decoder_free(&d);
}

static void test_output_buffer(void)
{
	static struct audio_rx a;
	static float out[48000 * 2];
	uint8_t pkt[960 * 2];
	struct audio_rx_stats st;
	size_t i;
	size_t first_audio;

	// 0.5 full-scale PCM16 mono, 960 frames per packet
	for (i = 0; i < 960; i++) {
		pkt[i * 2] = 0x00;
		pkt[i * 2 + 1] = 0x40;
	}

	audio_rx_init(&a, 48000);
	CHECK(audio_rx_configure(&a, AUDIO_CODEC_PCM16_1, 48000, 100) == 0);

	// Pre-fill: half of 100 ms at 48 kHz = 2400 frames of silence
	CHECK(a.target_frames == 4800);
	CHECK(atomic_load(&a.head) - atomic_load(&a.tail) == 2400);

	// Reading past it: silence and one underrun
	audio_rx_read(&a, out, 3000);
	CHECK(out[0] == 0.0f && out[2999 * 2] == 0.0f);
	audio_rx_get_stats(&a, &st);
	CHECK(st.underruns == 1);

	// Next packet: up to half the buffer of silence first, then audio on both channels
	audio_rx_push(&a, pkt, sizeof(pkt), 1000, 1000);
	CHECK(atomic_load(&a.head) - atomic_load(&a.tail) == 2400 + 960);
	audio_rx_read(&a, out, 2400 + 960);
	first_audio = 0;
	for (i = 0; i < 2400 + 960; i++) {
		if (out[i * 2] != 0.0f) {
			first_audio = i;
			break;
		}
	}
	CHECK(first_audio == 2400);
	CHECK(near(out[first_audio * 2], 16384.0 / 32767.0, 1e-4));
	CHECK(out[first_audio * 2] == out[first_audio * 2 + 1]);

	// Volume 0 -> silence; full -> unchanged
	audio_rx_set_volume(&a, 0);
	audio_rx_push(&a, pkt, sizeof(pkt), 1010, 1010);
	audio_rx_read(&a, out, 960);
	CHECK(out[100 * 2] == 0.0f);
	audio_rx_set_volume(&a, 255);

	// Overrun: more than the 4800-frame target is dropped
	for (i = 0; i < 8; i++)
		audio_rx_push(&a, pkt, sizeof(pkt), 1020, 1020);
	CHECK(atomic_load(&a.head) - atomic_load(&a.tail) == 4800);
	audio_rx_get_stats(&a, &st);
	CHECK(st.overrun_frames == 8 * 960 - 4800);

	// Late packet (> 1.5 x latency since arrival) is dropped
	audio_rx_push(&a, pkt, sizeof(pkt), 1000, 1151);
	audio_rx_get_stats(&a, &st);
	CHECK(st.dropped_old == 1);

	audio_rx_stop(&a);
}

int main(void)
{
	test_simple_codecs();
	test_resampler();
	test_opus_roundtrip();
	test_adpcm_roundtrip();
	test_output_buffer();

	if (G_FAILS != 0) {
		fprintf(stderr, "test_audio: %d failure(s)\n", G_FAILS);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
