// SPDX-License-Identifier: GPL-3.0-or-later
//
// Excess network delay estimate from peer ping timestamps.
//
// Port of PingDelayTracker from wfview v3-initial include/pingdelaytracker.h,
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//

#include "core/ping_delay.h"

#include <string.h>

//
// Normalise a ms value into 0 .. day-1
//

static int norm_day(int64_t ms)
{
	ms %= PING_DELAY_DAY_MS;
	if (ms < 0)
		ms += PING_DELAY_DAY_MS;

	return (int)ms;
}

//
// Shortest signed distance a - b on the day circle
//

static int signed_delta_day(int a, int b)
{
	int d = norm_day(a) - norm_day(b);

	if (d > PING_DELAY_DAY_MS / 2)
		d -= PING_DELAY_DAY_MS;
	if (d < -PING_DELAY_DAY_MS / 2)
		d += PING_DELAY_DAY_MS;

	return d;
}

//
// Forget all history
//

void ping_delay_reset(struct ping_delay *pd)
{
	memset(pd, 0, sizeof(*pd));
}

//
// Feed one ping; returns the current excess delay in ms (>= 0)
//

int ping_delay_update(struct ping_delay *pd, int64_t local_mono_ms, uint32_t remote_stamp_ms)
{
	const int remote = norm_day((int64_t)remote_stamp_ms);
	int64_t offset;
	int64_t ex;

	if (!pd->valid) {
		pd->valid = true;
		pd->last_remote = remote;
		pd->remote_unwrapped = remote;
		pd->baseline = local_mono_ms - pd->remote_unwrapped;
		pd->window_min = pd->baseline;
		pd->window_start = local_mono_ms;
		pd->excess = 0;
		return pd->excess;
	}

	pd->remote_unwrapped += signed_delta_day(remote, pd->last_remote);
	pd->last_remote = remote;

	offset = local_mono_ms - pd->remote_unwrapped;

	if (offset < pd->baseline)
		pd->baseline = offset;

	if (offset < pd->window_min)
		pd->window_min = offset;

	if (local_mono_ms - pd->window_start >= PING_DELAY_WINDOW_MS) {
		if (pd->window_min > pd->baseline)
			pd->baseline = pd->window_min;
		pd->window_min = offset;
		pd->window_start = local_mono_ms;
	}

	ex = offset - pd->baseline;
	if (ex < 0)
		ex = 0;
	if (ex > PING_DELAY_DAY_MS)
		ex = PING_DELAY_DAY_MS;

	pd->excess = (int)ex;
	return pd->excess;
}

//
// Excess delay above which audio is too late to play:
// one packet (20 ms) or half the latency, whichever is larger, on top of the latency
//

int ping_delay_drop_threshold_ms(int latency_ms)
{
	const int a = latency_ms + 20;
	const int b = latency_ms * 3 / 2;

	return a > b ? a : b;
}
