// SPDX-License-Identifier: GPL-3.0-or-later
//
// RX audio decoding: codec payload to interleaved float [-1, 1].
//
// Conversions as wfview v3-initial src/audio/audioconverter.cpp,
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//

#include "core/audio_decode.h"

#include <string.h>

#include "third_party/adpcm/adpcm-lib.h"
#include "third_party/opus/include/opus.h"

int audio_codec_channels(uint8_t codec)
{
	switch (codec) {
	case AUDIO_CODEC_PCM8_2:
	case AUDIO_CODEC_PCM16_2:
	case AUDIO_CODEC_ULAW_2:
	case AUDIO_CODEC_OPUS_2:
		return 2;
	case AUDIO_CODEC_ULAW_1:
	case AUDIO_CODEC_PCM8_1:
	case AUDIO_CODEC_PCM16_1:
	case AUDIO_CODEC_OPUS_1:
	case AUDIO_CODEC_ADPCM_1:
		return 1;
	default:
		return 0;
	}
}

int audio_decoder_init(struct audio_decoder *d, uint8_t codec, uint32_t sample_rate)
{
	memset(d, 0, sizeof(*d));
	d->codec = codec;
	d->channels = audio_codec_channels(codec);
	d->sample_rate = sample_rate;

	if (d->channels == 0)
		return -1;

	if (codec == AUDIO_CODEC_OPUS_1 || codec == AUDIO_CODEC_OPUS_2) {
		int err = 0;

		d->opus = opus_decoder_create((opus_int32)sample_rate, d->channels, &err);
		if (d->opus == NULL || err != OPUS_OK)
			return -1;
	}

	return 0;
}

void audio_decoder_free(struct audio_decoder *d)
{
	if (d->opus != NULL)
		opus_decoder_destroy(d->opus);
	d->opus = NULL;
}

//
// Bit-exact G.711 µ-law to float, as wfview
//

static float ulaw_to_float(uint8_t byte)
{
	const unsigned int u = (unsigned int)(~byte) & 0xffu;
	const int sign = (u & 0x80u) ? -1 : 1;
	const int exp = (int)((u >> 4) & 0x07u);
	const int mant = (int)(u & 0x0fu);
	const int t = ((mant << 3) + 0x84) << exp;
	float f = (float)((t - 0x84) * sign) / 32768.0f;

	if (f > 1.0f)
		f = 1.0f;
	if (f < -1.0f)
		f = -1.0f;
	return f;
}

//
// Decode one payload; returns frames written (per channel), negative on error.
// out holds frames * channels floats, interleaved as received.
//

int audio_decode(struct audio_decoder *d, const uint8_t *in, size_t len, float *out, size_t max_frames)
{
	const size_t ch = (size_t)d->channels;
	size_t i;
	size_t n;

	if (len == 0)
		return 0;

	switch (d->codec) {
	case AUDIO_CODEC_PCM16_1:
	case AUDIO_CODEC_PCM16_2:
		n = len / 2;
		if (n / ch > max_frames)
			n = max_frames * ch;
		for (i = 0; i < n; i++) {
			const int16_t s = (int16_t)(uint16_t)(in[2 * i] | (in[2 * i + 1] << 8));

			out[i] = (float)s / 32767.0f;
		}
		return (int)(n / ch);

	case AUDIO_CODEC_PCM8_1:
	case AUDIO_CODEC_PCM8_2:
		n = len;
		if (n / ch > max_frames)
			n = max_frames * ch;
		for (i = 0; i < n; i++)
			out[i] = ((float)in[i] - 128.0f) / 127.0f;
		return (int)(n / ch);

	case AUDIO_CODEC_ULAW_1:
	case AUDIO_CODEC_ULAW_2:
		n = len;
		if (n / ch > max_frames)
			n = max_frames * ch;
		for (i = 0; i < n; i++)
			out[i] = ulaw_to_float(in[i]);
		return (int)(n / ch);

	case AUDIO_CODEC_OPUS_1:
	case AUDIO_CODEC_OPUS_2: {
		const int ns = opus_packet_get_nb_samples(in, (opus_int32)len, (opus_int32)d->sample_rate);
		int ret;

		if (ns <= 0 || (size_t)ns > max_frames)
			return -1;
		ret = opus_decode_float(d->opus, in, (opus_int32)len, out, ns, 0);
		return ret < 0 ? -1 : ret;
	}

	case AUDIO_CODEC_ADPCM_1: {
		int16_t pcm[AUDIO_DECODE_MAX_FRAMES];
		int ns = adpcm_block_size_to_sample_count((int)len, 1, 4);
		int got;

		if (ns <= 0 || (size_t)ns > max_frames || (size_t)ns > AUDIO_DECODE_MAX_FRAMES)
			return -1;
		// Writes header sample + chunks * 8 but returns chunks * 8; wfview uses the
		// returned count (its buffer holds (len - 4) * 2 samples), so the last sample is dropped
		got = adpcm_decode_block(pcm, in, len, 1);
		for (i = 0; i < (size_t)got; i++)
			out[i] = (float)pcm[i] / 32767.0f;
		return got;
	}

	default:
		return -1;
	}
}
