// SPDX-License-Identifier: GPL-3.0-or-later
//
// Polyphase windowed-sinc resampler, arbitrary ratio, interleaved float.
//
// RESAMPLE_TAPS taps per phase when upsampling, RESAMPLE_TAPS / ratio when
// downsampling (so the transition band scales with the output rate), at most
// RESAMPLE_MAX_TAPS. RESAMPLE_PHASES phases with linear interpolation between
// neighbouring phases. Blackman-Harris window, cutoff at 0.45 of the lower
// of the two rates. Equal rates pass through unchanged.
//
// Position convention: pos is the fractional index, in input frames, of the
// next output sample relative to the start of the history buffer. The filter
// for output position p uses input frames floor(p) - taps/2 + 1 .. floor(p) + taps/2.
//

#include "core/resample.h"

#include <math.h>
#include <string.h>

#define C_PI 3.14159265358979323846

static double sinc(double x)
{
	if (fabs(x) < 1e-12)
		return 1.0;
	return sin(C_PI * x) / (C_PI * x);
}

static double blackman_harris(double n, double len)
{
	const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
	const double t = 2.0 * C_PI * n / (len - 1.0);

	return a0 - a1 * cos(t) + a2 * cos(2.0 * t) - a3 * cos(3.0 * t);
}

int resampler_init(struct resampler *r, uint32_t in_rate, uint32_t out_rate, int channels)
{
	double ratio;
	double cutoff;
	int taps;
	int p;
	int k;

	memset(r, 0, sizeof(*r));
	if (in_rate == 0 || out_rate == 0 || channels < 1 || channels > RESAMPLE_MAX_CH)
		return -1;

	ratio = (double)out_rate / (double)in_rate;

	// Lowpass at 0.45 of the lower rate, expressed in cycles per input sample x 2
	cutoff = 0.9 * (ratio < 1.0 ? ratio : 1.0);

	taps = RESAMPLE_TAPS;
	if (ratio < 1.0) {
		taps = (int)ceil(RESAMPLE_TAPS / ratio);
		taps += taps & 1;
		if (taps > RESAMPLE_MAX_TAPS)
			taps = RESAMPLE_MAX_TAPS;
	}

	r->in_rate = in_rate;
	r->out_rate = out_rate;
	r->channels = channels;
	r->taps = taps;
	r->step = (double)in_rate / (double)out_rate;

	// coef[p][k]: tap k for fractional offset p / PHASES, normalised to unity DC gain
	for (p = 0; p <= RESAMPLE_PHASES; p++) {
		const double frac = (double)p / RESAMPLE_PHASES;
		double sum = 0.0;

		for (k = 0; k < taps; k++) {
			const double x = (double)(k - (taps / 2 - 1)) - frac;
			const double w = blackman_harris(x + taps / 2.0, taps + 1.0);
			const double v = cutoff * sinc(cutoff * x) * w;

			r->coef[p][k] = (float)v;
			sum += v;
		}
		for (k = 0; k < taps; k++)
			r->coef[p][k] = (float)(r->coef[p][k] / sum);
	}

	resampler_reset(r);
	return 0;
}

void resampler_reset(struct resampler *r)
{
	const size_t half = (size_t)r->taps / 2;

	memset(r->hist, 0, sizeof(r->hist));
	// Start with taps/2 frames of silence so the first output is centred on real input
	r->hist_n = half;
	r->pos = (double)half;
}

size_t resampler_max_out(const struct resampler *r, size_t in_frames)
{
	return (size_t)((double)in_frames / r->step) + 2;
}

//
// Process in_frames frames; returns output frames written (at most max_out).
// Input beyond what fits in max_out is still consumed into the history.
//

size_t resampler_process(struct resampler *r, const float *in, size_t in_frames, float *out, size_t max_out)
{
	const size_t ch = (size_t)r->channels;
	const size_t taps = (size_t)r->taps;
	const size_t half = taps / 2;
	size_t produced = 0;
	size_t i = 0;

	if (r->in_rate == r->out_rate) {
		const size_t n = in_frames < max_out ? in_frames : max_out;

		memcpy(out, in, n * ch * sizeof(float));
		return n;
	}

	while (i < in_frames) {
		size_t space = RESAMPLE_HIST - r->hist_n;
		size_t take = in_frames - i;
		size_t base;

		// Append as much input as fits into the history
		if (take > space)
			take = space;
		memcpy(r->hist + r->hist_n * ch, in + i * ch, take * ch * sizeof(float));
		r->hist_n += take;
		i += take;

		// Produce while the filter window lies inside the history
		while ((size_t)r->pos + half < r->hist_n) {
			const size_t ip = (size_t)r->pos;
			const double fr = (r->pos - (double)ip) * RESAMPLE_PHASES;
			const size_t ph = (size_t)fr;
			const float a = (float)(fr - (double)ph);
			const float *c0 = r->coef[ph];
			const float *c1 = r->coef[ph + 1];
			const size_t first = ip - (half - 1);
			size_t c;
			size_t k;

			if (produced < max_out) {
				for (c = 0; c < ch; c++) {
					float acc = 0.0f;

					for (k = 0; k < taps; k++) {
						const float cf = c0[k] + a * (c1[k] - c0[k]);

						acc += cf * r->hist[(first + k) * ch + c];
					}
					out[produced * ch + c] = acc;
				}
				produced++;
			}
			r->pos += r->step;
		}

		// Drop history no longer needed, keep taps frames before pos
		base = (size_t)r->pos;
		if (base > taps) {
			const size_t drop = base - taps;

			memmove(r->hist, r->hist + drop * ch, (r->hist_n - drop) * ch * sizeof(float));
			r->hist_n -= drop;
			r->pos -= (double)drop;
		}
	}

	return produced;
}
