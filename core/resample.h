// SPDX-License-Identifier: GPL-3.0-or-later
//
// Polyphase windowed-sinc resampler, arbitrary ratio, interleaved float.
//

#pragma once

#include <stddef.h>
#include <stdint.h>

#define RESAMPLE_TAPS		32	// taps when upsampling; scaled by 1/ratio when downsampling
#define RESAMPLE_MAX_TAPS	192	// down to ratio 1/6 (48 kHz -> 8 kHz)
#define RESAMPLE_PHASES		256
#define RESAMPLE_MAX_CH		2
#define RESAMPLE_HIST		(2 * RESAMPLE_MAX_TAPS)

struct resampler {

	//
	// Configuration
	//
	uint32_t in_rate;
	uint32_t out_rate;
	int channels;
	int taps;
	double step;

	//
	// Filter bank: (PHASES + 1) x taps, row stride RESAMPLE_MAX_TAPS
	//
	float coef[RESAMPLE_PHASES + 1][RESAMPLE_MAX_TAPS];

	//
	// State: input history and fractional read position into it
	//
	size_t hist_n;
	float hist[RESAMPLE_HIST * RESAMPLE_MAX_CH];
	double pos;
};

//
// Lifecycle
//

int resampler_init(struct resampler *r, uint32_t in_rate, uint32_t out_rate, int channels);
void resampler_reset(struct resampler *r);

//
// Processing
//

size_t resampler_max_out(const struct resampler *r, size_t in_frames);
size_t resampler_process(struct resampler *r, const float *in, size_t in_frames, float *out, size_t max_out);
