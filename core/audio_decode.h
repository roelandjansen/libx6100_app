// SPDX-License-Identifier: GPL-3.0-or-later
//
// RX audio decoding: codec payload to interleaved float [-1, 1].
//
// Conversions as wfview main src/audio/audioconverter.cpp,
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//

#pragma once

#include <stddef.h>
#include <stdint.h>

//
// Codec IDs (wfview audioconverter.h)
//

#define AUDIO_CODEC_ULAW_1	0x01
#define AUDIO_CODEC_PCM8_1	0x02
#define AUDIO_CODEC_PCM16_1	0x04
#define AUDIO_CODEC_PCM8_2	0x08
#define AUDIO_CODEC_PCM16_2	0x10
#define AUDIO_CODEC_ULAW_2	0x20
#define AUDIO_CODEC_OPUS_1	0x40
#define AUDIO_CODEC_OPUS_2	0x41
#define AUDIO_CODEC_ADPCM_1	0x80

//
// Largest decoded packet: Opus 120 ms at 48 kHz stereo
//

#define AUDIO_DECODE_MAX_FRAMES	5760

struct audio_decoder {
	uint8_t codec;
	int channels;
	uint32_t sample_rate;
	void *opus;
};

//
// Lifecycle
//

int audio_decoder_init(struct audio_decoder *d, uint8_t codec, uint32_t sample_rate);
void audio_decoder_free(struct audio_decoder *d);
int audio_codec_channels(uint8_t codec);

//
// Decoding
//

int audio_decode(struct audio_decoder *d, const uint8_t *in, size_t len, float *out, size_t max_frames);
